#pragma once

#include "src/renderer/RenderCommand.h"
#include "src/resource/Asset.h"
#include "src/scene/DirectionalLightSource.h"
#include "src/scene/PointLightSource.h"
#include "src/scene/SpotLightSource.h"

#include <span>
#include <vector>

// Per-frame renderer input.  This is intentionally not a world/ECS container:
// callers submit the renderable state they want drawn for the current frame.
class SceneCollection final
{
  public:
	void BeginFrame(uint64 FrameNumber);
	void Submit(renderer::RenderItem Item);
	[[nodiscard]] uint32 AppendSkinningPalette(std::span<const glm::mat4> Current, std::span<const glm::mat4> Previous);
	[[nodiscard]] uint32 AppendMorphWeights(std::span<const renderer::GPUMorphWeightRecord> Weights);
	template <IsAsset T> void RetainAsset(resource::AssetPtr<T> Asset)
	{
		if (Asset != nullptr)
			this->AssetPins.emplace_back(std::move(Asset));
	}
	void AddDirectionalLight(const DirectionalLightSource &Light);
	void AddPointLight(const PointLightSource &Light);
	void AddSpotLight(const SpotLightSource &Light);
	void Seal();
	void Clear();
	[[nodiscard]] std::vector<resource::AssetPtr<resource::Asset>> ReleaseAssetPins() noexcept;

	[[nodiscard]] uint64 GetFrameNumber() const noexcept;
	[[nodiscard]] bool IsSealed() const noexcept;
	[[nodiscard]] const std::vector<renderer::RenderItem> &GetRenderItems() const noexcept;
	[[nodiscard]] const std::vector<DirectionalLightSource> &GetDirectionalLights() const noexcept;
	[[nodiscard]] const std::vector<PointLightSource> &GetPointLights() const noexcept;
	[[nodiscard]] const std::vector<SpotLightSource> &GetSpotLights() const noexcept;
	[[nodiscard]] const std::vector<renderer::GPUSkinMatrixRecord> &GetSkinningMatrices() const noexcept;
	[[nodiscard]] const std::vector<renderer::GPUMorphWeightRecord> &GetMorphWeights() const noexcept;

  private:
	uint64 FrameNumber = 0;
	bool Sealed = false;
	std::vector<renderer::RenderItem> RenderItems;
	std::vector<DirectionalLightSource> DirectionalLights;
	std::vector<PointLightSource> PointLights;
	std::vector<SpotLightSource> SpotLights;
	std::vector<renderer::GPUSkinMatrixRecord> SkinningMatrices;
	std::vector<renderer::GPUMorphWeightRecord> MorphWeights;
	std::vector<resource::AssetPtr<resource::Asset>> AssetPins;
};
