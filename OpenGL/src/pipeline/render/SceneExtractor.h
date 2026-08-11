#pragma once

#include "src/core/EngineAPI.h"

#include "src/pipeline/mesh/MeshGpuResource.h"
#include "src/pipeline/render/RenderData.h"
#include "src/pipeline/render/SceneRenderSnapshot.h"
#include "src/resource/asset/AssetManager.h"
#include "src/scene/Scene.h"
#include "src/scene/SceneCollection.h"

#include <glm.hpp>
#include <unordered_map>
#include <vector>

class Camera;

namespace pipeline::render
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

struct RenderTransformHistoryEntry final
{
	glm::mat4 Transform{1.0f};
	uint64 Generation = 0;
};

using RenderTransformHistory = std::unordered_map<RenderTransformHistoryKey, RenderTransformHistoryEntry, RenderTransformHistoryKeyHash>;

struct SceneExtractorScratch final
{
	struct SkinPaletteEntry final
	{
		uint32 Offset = 0;
		uint64 Generation = 0;
	};

	std::vector<glm::mat4> NodeTransforms;
	std::unordered_map<resource::AssetID, SkinPaletteEntry> SkinPaletteOffsets;
	std::vector<glm::mat4> FallbackGlobalPose;
	std::vector<glm::mat4> FallbackSkinPose;
	std::vector<GPUMorphWeightRecord> ActiveMorphWeights;
	SceneRenderSnapshot SceneSnapshot;
	SceneRenderSnapshotBuildScratch SceneSnapshotBuildScratch;
	uint64 SkinPaletteGeneration = 0;

	SceneExtractorScratch()
	{
		this->NodeTransforms.reserve(4'096);
		this->SkinPaletteOffsets.reserve(256);
		this->FallbackGlobalPose.reserve(4'096);
		this->FallbackSkinPose.reserve(4'096);
		this->ActiveMorphWeights.reserve(4'096);
	}
};

class ENGINE_API SceneExtractor final
{
  public:
	SceneExtractor(pipeline::device::Device &Device, pipeline::mesh::MeshGPUCache &MeshCache, resource::AssetManager &Assets,
				   const RenderTransformHistory &PreviousTransforms, uint64 PreviousGeneration, RenderTransformHistory &CurrentTransforms,
				   uint64 CurrentGeneration, SceneExtractorScratch &Scratch)
		: Device(Device), MeshCache(&MeshCache), Assets(&Assets), PreviousTransforms(&PreviousTransforms),
		  PreviousGeneration(PreviousGeneration), CurrentTransforms(&CurrentTransforms), CurrentGeneration(CurrentGeneration),
		  Scratch(&Scratch)
	{
	}

	void Extract(const world::Scene &Scene, const Camera &Camera, SceneCollection &Output) const;
	void Extract(const SceneRenderSnapshot &Snapshot, const Camera &Camera, SceneCollection &Output) const;

  private:
	pipeline::device::DeviceHandle Device;
	pipeline::mesh::MeshGPUCache *MeshCache = nullptr;
	resource::AssetManager *Assets = nullptr;
	const RenderTransformHistory *PreviousTransforms = nullptr;
	uint64 PreviousGeneration = 0;
	RenderTransformHistory *CurrentTransforms = nullptr;
	uint64 CurrentGeneration = 0;
	SceneExtractorScratch *Scratch = nullptr;

	[[nodiscard]] uint32 SelectLOD(const resource::MeshAsset &Mesh, const components::MeshLODPolicy &Policy,
								   const glm::mat4 &WorldTransform, const Camera &Camera) const;
	[[nodiscard]] resource::AssetHandle<resource::MaterialInterfaceAsset> ResolveMaterial(
		std::span<const components::MeshMaterialOverride> Overrides, resource::ModelMeshInstanceID MeshInstance,
		const resource::MeshMaterialSlot &Slot) const;
	[[nodiscard]] GPUMaterialRecord BuildMaterialRecord(const resource::AssetPtr<resource::MaterialInterfaceAsset> &Material,
														SceneCollection &Output) const;
};

// SceneExtractor is the render-thread GPU resolution stage described by the
// renderer contract. Keep the descriptive alias so callers can make the
// CPU-snapshot/GPU-resolution boundary explicit without duplicating logic.
using SceneGpuResolver = SceneExtractor;
} // namespace pipeline::render
