#pragma once

#include "Source/core/EngineAPI.h"

#include "Source/pipeline/render/PickTable.h"
#include "Source/pipeline/render/RenderCommand.h"
#include "Source/resource/Asset.h"
#include "Source/scene/DirectionalLightSource.h"
#include "Source/scene/PointLightSource.h"
#include "Source/scene/SpotLightSource.h"

#include <span>
#include <memory>
#include <stdexcept>
#include <vector>

// Per-frame renderer input.  This is intentionally not a world/ECS container:
// callers submit the renderable state they want drawn for the current frame.
struct SceneCollectionCapacitySpecification final
{
	uint32 RenderItems = 4'096;
	uint32 SkinningMatrices = 4'096;
	uint32 MorphWeights = 4'096;
	uint32 PickObjects = 4'096;
	uint32 DirectionalLights = 16;
	uint32 PointLights = 256;
	uint32 SpotLights = 256;
};

class ENGINE_API SceneCollection final
{
  public:
	explicit SceneCollection(SceneCollectionCapacitySpecification Specification = {});

	void BeginFrame(uint64 FrameNumber);
	[[nodiscard]] pipeline::render::PickID RegisterPickObject(world::ObjectHandle Object);
	void Submit(pipeline::render::RenderItem Item);
	[[nodiscard]] uint32 AppendSkinningPalette(std::span<const glm::mat4> Current, std::span<const glm::mat4> Previous);
	[[nodiscard]] uint32 AppendMorphWeights(std::span<const pipeline::render::GPUMorphWeightRecord> Weights);
	template <IsAsset T> void RetainAsset(resource::AssetPtr<T> Asset)
	{
		if (this->PickTable == nullptr)
			throw std::logic_error("Scene collection must begin a frame before retaining assets");
		if (Asset != nullptr)
			this->AssetPins.emplace_back(std::move(Asset));
	}
	void AddDirectionalLight(const DirectionalLightSource &Light);
	void AddPointLight(const PointLightSource &Light);
	void AddSpotLight(const SpotLightSource &Light);
	void Seal();
	void Clear();
	[[nodiscard]] std::vector<resource::AssetPtr<resource::Asset>> ReleaseAssetPins() noexcept;
	// Swaps pins into caller-owned frame-slot storage so both vectors retain their capacities.
	void ReleaseAssetPinsInto(std::vector<resource::AssetPtr<resource::Asset>> &Destination) noexcept;

	[[nodiscard]] uint64 GetFrameNumber() const noexcept;
	[[nodiscard]] bool IsSealed() const noexcept;
	[[nodiscard]] const std::vector<pipeline::render::RenderItem> &GetRenderItems() const noexcept;
	[[nodiscard]] const std::vector<DirectionalLightSource> &GetDirectionalLights() const noexcept;
	[[nodiscard]] const std::vector<PointLightSource> &GetPointLights() const noexcept;
	[[nodiscard]] const std::vector<SpotLightSource> &GetSpotLights() const noexcept;
	[[nodiscard]] const std::vector<pipeline::render::GPUSkinMatrixRecord> &GetSkinningMatrices() const noexcept;
	[[nodiscard]] const std::vector<pipeline::render::GPUMorphWeightRecord> &GetMorphWeights() const noexcept;
	[[nodiscard]] std::shared_ptr<const pipeline::render::FramePickTable> GetPickTable() const noexcept;

  private:
	uint64 FrameNumber = 0;
	bool Sealed = false;
	std::vector<pipeline::render::RenderItem> RenderItems;
	std::vector<DirectionalLightSource> DirectionalLights;
	std::vector<PointLightSource> PointLights;
	std::vector<SpotLightSource> SpotLights;
	std::vector<pipeline::render::GPUSkinMatrixRecord> SkinningMatrices;
	std::vector<pipeline::render::GPUMorphWeightRecord> MorphWeights;
	std::vector<resource::AssetPtr<resource::Asset>> AssetPins;
	std::shared_ptr<pipeline::render::FramePickTable> PickTable;
	std::vector<std::shared_ptr<pipeline::render::FramePickTable>> PickTablePool;
	SceneCollectionCapacitySpecification Capacity;
};
