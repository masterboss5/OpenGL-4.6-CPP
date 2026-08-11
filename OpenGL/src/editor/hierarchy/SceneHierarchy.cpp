#include "SceneHierarchy.h"

#include "src/component/object/CObjectHierarchyComponent.h"
#include "src/component/object/CObjectIdentityComponent.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace editor::hierarchy
{
namespace
{
struct ObjectHandleHash final
{
	[[nodiscard]] usize operator()(const world::ObjectHandle &Handle) const noexcept
	{
		uint64 Hash = 1469598103934665603ULL;
		const auto Mix = [&Hash](const uint64 Value)
		{
			Hash ^= Value;
			Hash *= 1099511628211ULL;
		};
		Mix(Handle.Scene);
		Mix(Handle.Slot);
		Mix(Handle.Generation);
		return static_cast<usize>(Hash);
	}
};

struct SourceNode final
{
	util::UUID PersistentID;
	world::ObjectHandle Object;
	world::ObjectHandle Parent;
	string Name;
	std::vector<string> Tags;
	uint32 SiblingOrder = 0;
	components::ObjectMobility Mobility = components::ObjectMobility::Movable;
	bool Enabled = true;
	bool EditorVisible = true;
	bool Locked = false;
};

[[nodiscard]] string NormalizeQuery(const string_view Query)
{
	string Normalized;
	Normalized.reserve(Query.size());
	for (const string::value_type Character : Query)
		Normalized.push_back(static_cast<string::value_type>(std::tolower(static_cast<unsigned char>(Character))));
	return Normalized;
}
} // namespace

SceneHierarchySnapshot SceneHierarchyBuilder::Build(const world::Scene &Scene, const uint64 SceneRevision)
{
	std::vector<SourceNode> Sources;
	{
		auto Access = Scene.Read();
		const std::vector<world::ObjectHandle> Objects = Access.Objects();
		Sources.reserve(Objects.size());
		for (const world::ObjectHandle Object : Objects)
		{
			const world::ComponentHandle<components::CObjectIdentityComponent> Identity =
				Access.GetComponent<components::CObjectIdentityComponent>(Object);
			if (!Identity.IsValid())
				throw std::logic_error("Editor scene hierarchy encountered an object without a persistent identity component");
			const components::CObjectIdentityComponent &IdentityComponent = Access.Resolve(Identity);

			world::ObjectHandle Parent;
			uint32 SiblingOrder = 0;
			const world::ComponentHandle<components::CObjectHierarchyComponent> Hierarchy =
				Access.GetComponent<components::CObjectHierarchyComponent>(Object);
			if (Hierarchy.IsValid())
			{
				const components::CObjectHierarchyComponent &HierarchyComponent = Access.Resolve(Hierarchy);
				Parent = HierarchyComponent.GetParent();
				SiblingOrder = HierarchyComponent.GetSiblingOrder();
			}
			Sources.push_back({.PersistentID = IdentityComponent.GetPersistentID(),
							   .Object = Object,
							   .Parent = Parent,
							   .Name = IdentityComponent.GetName(),
							   .Tags = {IdentityComponent.GetTags().begin(), IdentityComponent.GetTags().end()},
							   .SiblingOrder = SiblingOrder,
							   .Mobility = IdentityComponent.GetMobility(),
							   .Enabled = IdentityComponent.IsEnabled(),
							   .EditorVisible = IdentityComponent.IsEditorVisible(),
							   .Locked = IdentityComponent.IsLocked()});
		}
	}

	std::unordered_map<world::ObjectHandle, uint32, ObjectHandleHash> Indices;
	Indices.reserve(Sources.size());
	for (uint32 Index = 0; Index < Sources.size(); ++Index)
	{
		if (!Indices.emplace(Sources[Index].Object, Index).second)
			throw std::logic_error("Editor scene hierarchy captured a duplicate live object handle");
	}

	std::vector<std::vector<uint32>> Children(Sources.size());
	std::vector<uint32> Roots;
	Roots.reserve(Sources.size());
	for (uint32 Index = 0; Index < Sources.size(); ++Index)
	{
		if (!Sources[Index].Parent.IsValid())
		{
			Roots.push_back(Index);
			continue;
		}
		const auto Parent = Indices.find(Sources[Index].Parent);
		if (Parent == Indices.end())
			throw std::logic_error("Editor scene hierarchy captured an object whose parent is not present");
		Children[Parent->second].push_back(Index);
	}

	const auto SortSiblings = [&Sources](std::vector<uint32> &Siblings)
	{
		std::sort(Siblings.begin(), Siblings.end(),
				  [&Sources](const uint32 Left, const uint32 Right)
				  {
					  if (Sources[Left].SiblingOrder != Sources[Right].SiblingOrder)
						  return Sources[Left].SiblingOrder < Sources[Right].SiblingOrder;
					  if (Sources[Left].Name != Sources[Right].Name)
						  return Sources[Left].Name < Sources[Right].Name;
					  return Sources[Left].PersistentID < Sources[Right].PersistentID;
				  });
	};
	SortSiblings(Roots);
	for (std::vector<uint32> &Siblings : Children)
		SortSiblings(Siblings);

	struct StackEntry final
	{
		uint32 Source = 0;
		uint32 Depth = 0;
		uint32 ParentRow = InvalidHierarchyRow;
	};
	std::vector<StackEntry> Stack;
	Stack.reserve(Sources.size());
	for (auto Root = Roots.rbegin(); Root != Roots.rend(); ++Root)
		Stack.push_back({.Source = *Root});

	SceneHierarchySnapshot Snapshot{.SceneRevision = SceneRevision};
	Snapshot.Rows.reserve(Sources.size());
	std::vector<bool> Visited(Sources.size(), false);
	while (!Stack.empty())
	{
		const StackEntry Entry = Stack.back();
		Stack.pop_back();
		if (Visited[Entry.Source])
			throw std::logic_error("Editor scene hierarchy contains a parent cycle");
		Visited[Entry.Source] = true;

		const SourceNode &Source = Sources[Entry.Source];
		const uint32 Row = static_cast<uint32>(Snapshot.Rows.size());
		Snapshot.Rows.push_back({.PersistentID = Source.PersistentID,
								 .Object = Source.Object,
								 .Name = Source.Name,
								 .Tags = Source.Tags,
								 .Depth = Entry.Depth,
								 .ParentRow = Entry.ParentRow,
								 .ChildCount = static_cast<uint32>(Children[Entry.Source].size()),
								 .SiblingOrder = Source.SiblingOrder,
								 .Mobility = Source.Mobility,
								 .Enabled = Source.Enabled,
								 .EditorVisible = Source.EditorVisible,
								 .Locked = Source.Locked});
		const std::vector<uint32> &ChildRows = Children[Entry.Source];
		for (auto Child = ChildRows.rbegin(); Child != ChildRows.rend(); ++Child)
			Stack.push_back({.Source = *Child, .Depth = Entry.Depth + 1, .ParentRow = Row});
	}
	if (Snapshot.Rows.size() != Sources.size())
		throw std::logic_error("Editor scene hierarchy contains objects unreachable from any root");
	return Snapshot;
}

std::future<SceneHierarchySnapshot> SceneHierarchyBuilder::BuildAsync(core::threading::TaskScheduler &Scheduler, const world::Scene &Scene,
																	  const uint64 SceneRevision)
{
	return Scheduler.Submit([&Scene, SceneRevision]() { return SceneHierarchyBuilder::Build(Scene, SceneRevision); },
							core::threading::TaskPriority::Background);
}

SceneHierarchySnapshot SceneHierarchyBuilder::Filter(const SceneHierarchySnapshot &Source, const string_view Query)
{
	const string Normalized = NormalizeQuery(Query);
	if (Normalized.empty())
		return Source;

	std::vector<bool> Included(Source.Rows.size(), false);
	for (uint32 Row = 0; Row < Source.Rows.size(); ++Row)
	{
		const string Name = NormalizeQuery(Source.Rows[Row].Name);
		const bool TagMatches = std::ranges::any_of(Source.Rows[Row].Tags, [&Normalized](const string &Tag)
													{ return NormalizeQuery(Tag).find(Normalized) != string::npos; });
		if (Name.find(Normalized) == string::npos && !TagMatches)
			continue;
		uint32 Ancestor = Row;
		while (Ancestor != InvalidHierarchyRow && !Included[Ancestor])
		{
			Included[Ancestor] = true;
			Ancestor = Source.Rows[Ancestor].ParentRow;
		}
	}

	SceneHierarchySnapshot Result{.SceneRevision = Source.SceneRevision};
	Result.Rows.reserve(Source.Rows.size());
	std::vector<uint32> Remap(Source.Rows.size(), InvalidHierarchyRow);
	for (uint32 Row = 0; Row < Source.Rows.size(); ++Row)
	{
		if (!Included[Row])
			continue;
		SceneHierarchyRow Copy = Source.Rows[Row];
		Copy.ParentRow = Copy.ParentRow == InvalidHierarchyRow ? InvalidHierarchyRow : Remap[Copy.ParentRow];
		Copy.Depth = Copy.ParentRow == InvalidHierarchyRow ? 0 : Result.Rows[Copy.ParentRow].Depth + 1;
		Copy.ChildCount = 0;
		Remap[Row] = static_cast<uint32>(Result.Rows.size());
		Result.Rows.push_back(std::move(Copy));
		if (Result.Rows.back().ParentRow != InvalidHierarchyRow)
			++Result.Rows[Result.Rows.back().ParentRow].ChildCount;
	}
	return Result;
}
} // namespace editor::hierarchy
