#pragma once
#include "RendererGpuTypes.h"
#include "src/scene/DirectionalLightSource.h"
#include "src/scene/PointLightSource.h"
#include "src/scene/SpotLightSource.h"

#include <span>
#include <vector>

class SceneCollection;
// 4.3K fps avg before light refactor
class LightBufferManager final
{
  private:
	uint32 MaxLights = 0;
	std::vector<PointLightSource> PointLightSources;
	std::vector<SpotLightSource> SpotLightSources;
	std::vector<DirectionalLightSource> DirectionalLightSources;
	std::vector<renderer::GPULightRecord> GPURecords;
	[[nodiscard]] std::vector<renderer::GPULightRecord> BuildGPURecords(const std::vector<PointLightSource> &PointLights,
																		const std::vector<SpotLightSource> &SpotLights,
																		const std::vector<DirectionalLightSource> &DirectionalLights) const;

  public:
	explicit LightBufferManager(usize MaxLights);
	~LightBufferManager() = default;
	LightBufferManager() = delete;

	[[nodiscard]] uint32 GetTotalLightSourceCount() const;
	[[nodiscard]] const std::vector<PointLightSource> &GetPointLights() const noexcept;
	[[nodiscard]] const std::vector<SpotLightSource> &GetSpotLights() const noexcept;
	[[nodiscard]] const std::vector<DirectionalLightSource> &GetDirectionalLights() const noexcept;
	[[nodiscard]] std::span<const renderer::GPULightRecord> GetGPURecords() const noexcept;
	void Clear();
	void UploadLightSources(const std::vector<PointLightSource> &LightSources);
	void UploadLightSources(const std::vector<SpotLightSource> &LightSources);
	void UploadLightSources(const std::vector<DirectionalLightSource> &LightSources);
	void UploadSceneLights(const SceneCollection &Scene);

	LightBufferManager(const LightBufferManager &) = delete;
	LightBufferManager &operator=(const LightBufferManager &) = delete;
};
