#include "SceneTransformSnapshot.h"

#include "src/component/object/CObjectHierarchyComponent.h"
#include "src/component/object/CObjectTransformComponent.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace world
{
SceneTransformSnapshot SceneTransformSnapshot::Build(const Scene::ReadAccess &Access)
{
	SceneTransformSnapshotBuildScratch Scratch;
	SceneTransformSnapshot Result;
	SceneTransformSnapshot::BuildInto(Access, Result, Scratch);
	return Result;
}

void SceneTransformSnapshot::BuildInto(const Scene::ReadAccess &Access, SceneTransformSnapshot &Result,
									   SceneTransformSnapshotBuildScratch &Scratch)
{
	Access.ObjectsInto(Scratch.Objects);
	Scratch.Sources.clear();
	Scratch.Sources.reserve(Scratch.Objects.size());
	++Scratch.IndexGeneration;
	if (Scratch.IndexGeneration == 0)
	{
		Scratch.Indices.clear();
		Scratch.IndexGeneration = 1;
	}
	const uint64 IndexGeneration = Scratch.IndexGeneration;
	Scratch.Indices.reserve(Scratch.Objects.size());
	for (const ObjectHandle Object : Scratch.Objects)
	{
		const auto Transform = Access.GetComponent<components::CObjectTransformComponent>(Object);
		if (!Transform.IsValid())
			throw std::logic_error("Scene object has no transform component");
		ObjectHandle Parent;
		const auto Hierarchy = Access.GetComponent<components::CObjectHierarchyComponent>(Object);
		if (Hierarchy.IsValid())
			Parent = Access.Resolve(Hierarchy).GetParent();
		const uint32 Index = static_cast<uint32>(Scratch.Sources.size());
		auto [IndexEntry, Inserted] = Scratch.Indices.try_emplace(Object);
		if (!Inserted && IndexEntry->second.Generation == IndexGeneration)
			throw std::logic_error("Scene transform snapshot encountered a duplicate object handle");
		IndexEntry->second = {.Index = Index, .Generation = IndexGeneration};
		Scratch.Sources.push_back({.Object = Object, .Parent = Parent, .Local = Access.Resolve(Transform).GetMatrix()});
	}
	if (Scratch.Indices.size() > Scratch.Objects.size() * 2U)
	{
		std::erase_if(Scratch.Indices, [IndexGeneration](const auto &Entry) { return Entry.second.Generation != IndexGeneration; });
	}

	Scratch.Chain.clear();
	Scratch.Chain.reserve(Scratch.Sources.size());
	for (uint32 Start = 0; Start < Scratch.Sources.size(); ++Start)
	{
		if (Scratch.Sources[Start].State == SceneTransformResolutionState::Resolved)
			continue;
		Scratch.Chain.clear();
		uint32 Current = Start;
		while (true)
		{
			SceneTransformSource &Source = Scratch.Sources[Current];
			if (Source.State == SceneTransformResolutionState::Resolved)
				break;
			if (Source.State == SceneTransformResolutionState::Visiting)
				throw std::logic_error("Scene transform hierarchy contains a cycle");
			Source.State = SceneTransformResolutionState::Visiting;
			Scratch.Chain.push_back(Current);
			if (!Source.Parent.IsValid())
				break;
			const auto Parent = Scratch.Indices.find(Source.Parent);
			if (Parent == Scratch.Indices.end() || Parent->second.Generation != IndexGeneration)
				throw std::logic_error("Scene transform hierarchy references a missing parent");
			Current = Parent->second.Index;
		}

		while (!Scratch.Chain.empty())
		{
			const uint32 Index = Scratch.Chain.back();
			Scratch.Chain.pop_back();
			SceneTransformSource &Source = Scratch.Sources[Index];
			if (Source.Parent.IsValid())
			{
				const auto Parent = Scratch.Indices.find(Source.Parent);
				if (Parent == Scratch.Indices.end() || Parent->second.Generation != IndexGeneration ||
					Scratch.Sources[Parent->second.Index].State != SceneTransformResolutionState::Resolved)
					throw std::logic_error("Scene transform hierarchy could not resolve its parent chain");
				Source.World = Scratch.Sources[Parent->second.Index].World * Source.Local;
			}
			else
				Source.World = Source.Local;
			Source.State = SceneTransformResolutionState::Resolved;
		}
	}

	++Result.MatrixGeneration;
	if (Result.MatrixGeneration == 0)
	{
		Result.Matrices.clear();
		Result.MatrixGeneration = 1;
	}
	Result.MatrixCount = 0;
	Result.Matrices.reserve(Scratch.Sources.size());
	for (const SceneTransformSource &Source : Scratch.Sources)
	{
		Result.Matrices.insert_or_assign(
			Source.Object, SceneTransformSnapshot::MatrixEntry{.Matrix = Source.World, .Generation = Result.MatrixGeneration});
		++Result.MatrixCount;
	}
	if (Result.Matrices.size() > Scratch.Sources.size() * 2U)
	{
		const uint64 PublishedGeneration = Result.MatrixGeneration;
		std::erase_if(Result.Matrices, [PublishedGeneration](const auto &Entry) { return Entry.second.Generation != PublishedGeneration; });
	}
}

const glm::mat4 &SceneTransformSnapshot::GetMatrix(const ObjectHandle Object) const
{
	const auto Transform = this->Matrices.find(Object);
	if (Transform == this->Matrices.end() || Transform->second.Generation != this->MatrixGeneration)
		throw std::out_of_range("Scene transform snapshot does not contain the requested object");
	return Transform->second.Matrix;
}

glm::vec3 SceneTransformSnapshot::GetPosition(const ObjectHandle Object) const
{
	return glm::vec3(this->GetMatrix(Object)[3]);
}

glm::vec3 SceneTransformSnapshot::GetForward(const ObjectHandle Object) const
{
	const glm::vec3 Direction = glm::mat3(this->GetMatrix(Object)) * glm::vec3(0.0f, 0.0f, -1.0f);
	const float32 LengthSquared = glm::dot(Direction, Direction);
	if (LengthSquared <= 0.0f)
		throw std::logic_error("Scene world transform produced a degenerate forward direction");
	return Direction * glm::inversesqrt(LengthSquared);
}

usize SceneTransformSnapshot::Size() const noexcept
{
	return this->MatrixCount;
}

} // namespace world
