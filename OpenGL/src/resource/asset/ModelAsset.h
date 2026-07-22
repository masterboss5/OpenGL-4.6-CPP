#pragma once

#include "src/resource/Asset.h"
#include "src/resource/asset/AnimationAsset.h"
#include "src/resource/asset/AssetHandle.h"
#include "src/resource/asset/MeshAsset.h"

#include <glm.hpp>
#include <limits>
#include <span>
#include <vector>

namespace resource
{
using ModelNodeID = uint64;
using ModelMeshInstanceID = uint64;
inline constexpr uint32 InvalidModelNodeIndex = std::numeric_limits<uint32>::max();

struct ModelNode final
{
	ModelNodeID ID = 0;
	string Name;
	uint32 ParentIndex = InvalidModelNodeIndex;
	glm::mat4 LocalTransform{1.0f};
};

struct ModelMeshInstance final
{
	ModelMeshInstanceID ID = 0;
	uint32 NodeIndex = InvalidModelNodeIndex;
	AssetHandle<MeshAsset> Mesh;
};

class ModelAsset final : public Asset
{
  public:
	inline static constexpr resource::AssetType AssetType = resource::AssetType::Model;
	ModelAsset(string Name, Bounds Bounds, std::vector<ModelNode> Nodes, std::vector<ModelMeshInstance> MeshInstances,
			   std::vector<AssetHandle<AnimationClipAsset>> AnimationClips = {},
			   AssetHandle<AnimationGraphAsset> DefaultAnimationGraph = {});

	[[nodiscard]] string_view GetName() const noexcept
	{
		return this->Name;
	}
	[[nodiscard]] const Bounds &GetBounds() const noexcept
	{
		return this->Bounds;
	}
	[[nodiscard]] std::span<const ModelNode> GetNodes() const noexcept
	{
		return this->Nodes;
	}
	[[nodiscard]] std::span<const ModelMeshInstance> GetMeshInstances() const noexcept
	{
		return this->MeshInstances;
	}
	[[nodiscard]] std::span<const AssetHandle<AnimationClipAsset>> GetAnimationClips() const noexcept
	{
		return this->AnimationClips;
	}
	[[nodiscard]] const AssetHandle<AnimationGraphAsset> &GetDefaultAnimationGraph() const noexcept
	{
		return this->DefaultAnimationGraph;
	}
	[[nodiscard]] const ModelMeshInstance *FindMeshInstance(ModelMeshInstanceID ID) const noexcept;

  private:
	string Name;
	Bounds Bounds;
	std::vector<ModelNode> Nodes;
	std::vector<ModelMeshInstance> MeshInstances;
	std::vector<AssetHandle<AnimationClipAsset>> AnimationClips;
	AssetHandle<AnimationGraphAsset> DefaultAnimationGraph;
};
} // namespace resource
