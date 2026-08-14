#include "SceneHierarchy.h"

#include "Source/component/object/CObjectHierarchyComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"

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

SceneHierarchySnapshot SceneHierarchyBuilder::Build(const instance::InstanceGraph &Graph)
{
	const instance::InstanceGraphSnapshot Source = Graph.Snapshot();
	SceneHierarchySnapshot Result{.SceneRevision = Source.Revision};
	Result.Rows.reserve(Source.Instances.size());
	std::unordered_map<util::UUID, uint32> RowsByID;
	RowsByID.reserve(Source.Instances.size());
	for (const instance::InstanceRecord &Record : Source.Instances)
	{
		const uint32 ParentRow = Record.Parent.IsValid() ? RowsByID.at(Record.Parent) : InvalidHierarchyRow;
		const uint32 Row = static_cast<uint32>(Result.Rows.size());
		Result.Rows.push_back({.PersistentID = Record.ID,
							   .Name = Record.Name,
							   .ClassID = Record.ClassID,
							   .ClassName = Record.ClassName,
							   .Activation = Graph.GetActivation(Record.ID),
							   .Protected = Record.Protected,
							   .Depth = ParentRow == InvalidHierarchyRow ? 0U : Result.Rows[ParentRow].Depth + 1U,
							   .ParentRow = ParentRow,
							   .ChildCount = static_cast<uint32>(Record.Children.size()),
							   .SiblingOrder = Record.SiblingOrder,
							   .Enabled = Record.Enabled,
							   .EditorVisible = true,
							   .Locked = Record.Protected});
		RowsByID.emplace(Record.ID, Row);
	}
	return Result;
}

std::future<SceneHierarchySnapshot> SceneHierarchyBuilder::BuildAsync(core::threading::TaskScheduler &Scheduler, const world::Scene &Scene,
																	  const uint64 SceneRevision)
{
	return Scheduler.Submit([&Scene, SceneRevision]() { return SceneHierarchyBuilder::Build(Scene, SceneRevision); },
							core::threading::TaskPriority::Background);
}

std::future<SceneHierarchySnapshot> SceneHierarchyBuilder::BuildAsync(core::threading::TaskScheduler &Scheduler,
																	  const instance::InstanceGraph &Graph)
{
	const instance::InstanceGraphSnapshot Source = Graph.Snapshot();
	const std::vector<instance::InstanceTypeDescriptor> Types = Graph.GetTypes().GetTypes();
	return Scheduler.Submit(
		[Source, Types]()
		{
			std::unordered_map<instance::InstanceClassID, instance::InstanceTypeDescriptor> Descriptors;
			std::unordered_map<util::UUID, const instance::InstanceRecord *> Records;
			Descriptors.reserve(Types.size());
			for (const auto &Descriptor : Types)
				Descriptors.emplace(Descriptor.ClassID, Descriptor);
			Records.reserve(Source.Instances.size());
			for (const instance::InstanceRecord &Record : Source.Instances)
				Records.emplace(Record.ID, &Record);
			SceneHierarchySnapshot Result{.SceneRevision = Source.Revision};
			Result.Rows.reserve(Source.Instances.size());
			std::unordered_map<util::UUID, uint32> RowsByID;
			RowsByID.reserve(Source.Instances.size());
			for (const instance::InstanceRecord &Record : Source.Instances)
			{
				const uint32 ParentRow = Record.Parent.IsValid() ? RowsByID.at(Record.Parent) : InvalidHierarchyRow;
				instance::InstanceActivation Activation{.State = instance::InstanceActivationState::Active};
				const auto Descriptor = Descriptors.find(Record.ClassID);
				if (Descriptor == Descriptors.end())
					Activation = {.State = instance::InstanceActivationState::Inactive, .Diagnostic = "Instance class is not registered"};
				else if (Descriptor->second.Availability == instance::InstanceAvailability::Unavailable)
					Activation = {.State = instance::InstanceActivationState::Unavailable,
								  .Diagnostic = "This instance class is not implemented yet"};
				else if (!Record.Enabled)
					Activation = {.State = instance::InstanceActivationState::Inactive, .Diagnostic = "Instance is disabled"};
				else
				{
					if (!Descriptor->second.AllowedServiceClasses.empty())
					{
						const instance::InstanceRecord *Root = &Record;
						while (Root->Parent.IsValid())
						{
							const auto Parent = Records.find(Root->Parent);
							if (Parent == Records.end())
							{
								Activation = {.State = instance::InstanceActivationState::Inactive,
											  .Diagnostic = "Instance hierarchy is incomplete"};
								break;
							}
							Root = Parent->second;
						}
						if (Activation.State == instance::InstanceActivationState::Active &&
							std::ranges::find(Descriptor->second.AllowedServiceClasses, Root->ClassID) ==
								Descriptor->second.AllowedServiceClasses.end())
						{
							Activation = {.State = instance::InstanceActivationState::Inactive,
										  .Diagnostic = "Instance is outside a compatible service hierarchy"};
						}
					}
					if (Activation.State == instance::InstanceActivationState::Active && !Descriptor->second.ExactParentClasses.empty())
					{
						const auto Parent = Records.find(Record.Parent);
						const bool ValidParent =
							Parent != Records.end() && std::ranges::find(Descriptor->second.ExactParentClasses, Parent->second->ClassID) !=
														   Descriptor->second.ExactParentClasses.end();
						if (!ValidParent)
							Activation = {.State = instance::InstanceActivationState::Inactive,
										  .Diagnostic = "Immediate parent is incompatible with " + Descriptor->second.DisplayName};
					}
					if (Activation.State == instance::InstanceActivationState::Active &&
						Record.ClassID == instance::class_ids::AnimationTrack)
					{
						const auto Clip = Record.Properties.find("Clip");
						if (Clip == Record.Properties.end() || !std::holds_alternative<instance::InstanceAssetReference>(Clip->second) ||
							std::get<instance::InstanceAssetReference>(Clip->second).ID.empty())
						{
							Activation = {.State = instance::InstanceActivationState::Inactive,
										  .Diagnostic = "AnimationTrack requires exactly one AnimationClip asset"};
						}
					}
					if (Activation.State == instance::InstanceActivationState::Active && Record.ClassID == instance::class_ids::Script)
					{
						const auto StableType = Record.Properties.find("StableTypeID");
						if (StableType == Record.Properties.end() || !std::holds_alternative<util::UUID>(StableType->second) ||
							!std::get<util::UUID>(StableType->second).IsValid())
						{
							Activation = {.State = instance::InstanceActivationState::Inactive,
										  .Diagnostic = "Script requires a registered behavior type"};
						}
					}
				}
				const uint32 Row = static_cast<uint32>(Result.Rows.size());
				Result.Rows.push_back({.PersistentID = Record.ID,
									   .Name = Record.Name,
									   .ClassID = Record.ClassID,
									   .ClassName = Record.ClassName,
									   .Activation = std::move(Activation),
									   .Protected = Record.Protected,
									   .Depth = ParentRow == InvalidHierarchyRow ? 0U : Result.Rows[ParentRow].Depth + 1U,
									   .ParentRow = ParentRow,
									   .ChildCount = static_cast<uint32>(Record.Children.size()),
									   .SiblingOrder = Record.SiblingOrder,
									   .Enabled = Record.Enabled,
									   .EditorVisible = true,
									   .Locked = Record.Protected});
				RowsByID.emplace(Record.ID, Row);
			}
			return Result;
		},
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
