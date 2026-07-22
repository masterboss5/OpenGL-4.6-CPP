#pragma once

#include "src/renderer/MeshGpuResource.h"
#include "src/resource/asset/AssetManager.h"
#include "src/scene/Scene.h"
#include "src/scene/SceneCollection.h"

#include <unordered_map>

class Camera;

namespace renderer
{
struct RenderTransformHistoryKey final
{
	uint64 Scene = 0;
	uint32 ObjectSlot = 0;
	uint32 ObjectGeneration = 0;
	uint64 MeshInstance = 0;

	[[nodiscard]] bool operator==(const RenderTransformHistoryKey &) const noexcept = default;
};

struct RenderTransformHistoryKeyHash final
{
	[[nodiscard]] usize operator()(const RenderTransformHistoryKey &Key) const noexcept;
};

using RenderTransformHistory = std::unordered_map<RenderTransformHistoryKey, glm::mat4, RenderTransformHistoryKeyHash>;

class SceneExtractor final
{
  public:
	SceneExtractor(pipeline::device::Device &Device, MeshGPUCache &MeshCache, resource::AssetManager &Assets,
				   const RenderTransformHistory &PreviousTransforms, RenderTransformHistory &CurrentTransforms)
		: Device(&Device), MeshCache(&MeshCache), Assets(&Assets), PreviousTransforms(&PreviousTransforms),
		  CurrentTransforms(&CurrentTransforms)
	{
	}

	void Extract(const world::Scene &Scene, const Camera &Camera, SceneCollection &Output) const;

  private:
	pipeline::device::Device *Device = nullptr;
	MeshGPUCache *MeshCache = nullptr;
	resource::AssetManager *Assets = nullptr;
	const RenderTransformHistory *PreviousTransforms = nullptr;
	RenderTransformHistory *CurrentTransforms = nullptr;

	[[nodiscard]] uint32 SelectLOD(const resource::MeshAsset &Mesh, const components::MeshLODPolicy &Policy,
								   const glm::mat4 &WorldTransform, const Camera &Camera) const;
	[[nodiscard]] resource::AssetHandle<resource::MaterialInterfaceAsset> ResolveMaterial(const components::CObjectMeshComponent &Component,
																						  resource::ModelMeshInstanceID MeshInstance,
																						  const resource::MeshMaterialSlot &Slot) const;
	[[nodiscard]] GPUMaterialRecord BuildMaterialRecord(const resource::AssetPtr<resource::MaterialInterfaceAsset> &Material,
														SceneCollection &Output) const;
};
} // namespace renderer
