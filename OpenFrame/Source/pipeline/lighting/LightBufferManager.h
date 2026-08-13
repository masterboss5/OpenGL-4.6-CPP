#pragma once

#include "Source/core/EngineAPI.h"
#include "Source/pipeline/render/RenderData.h"
#include "Source/scene/DirectionalLightSource.h"
#include "Source/scene/PointLightSource.h"
#include "Source/scene/SpotLightSource.h"

#include <span>
#include <vector>

class SceneCollection;

namespace pipeline::lighting
{
// 4.3K fps avg before light refactor
class ENGINE_API LightBufferManager final
{
  private:
	uint32 MaxLights = 0;
	std::vector<PointLightSource> PointLightSources;
	std::vector<SpotLightSource> SpotLightSources;
	std::vector<DirectionalLightSource> DirectionalLightSources;
	std::vector<pipeline::render::GPULightRecord> GPURecords;
	std::vector<PointLightSource> ProposedPointLightSources;
	std::vector<SpotLightSource> ProposedSpotLightSources;
	std::vector<DirectionalLightSource> ProposedDirectionalLightSources;
	std::vector<pipeline::render::GPULightRecord> ProposedGPURecords;
	[[nodiscard]] std::vector<pipeline::render::GPULightRecord> BuildGPURecords(
		const std::vector<PointLightSource> &PointLights, const std::vector<SpotLightSource> &SpotLights,
		const std::vector<DirectionalLightSource> &DirectionalLights) const;
	void BuildGPURecordsInto(const std::vector<PointLightSource> &PointLights, const std::vector<SpotLightSource> &SpotLights,
							 const std::vector<DirectionalLightSource> &DirectionalLights,
							 std::vector<pipeline::render::GPULightRecord> &Records) const;

  public:
	explicit LightBufferManager(usize MaxLights);
	~LightBufferManager() = default;
	LightBufferManager() = delete;

	[[nodiscard]] uint32 GetTotalLightSourceCount() const;
	[[nodiscard]] const std::vector<PointLightSource> &GetPointLights() const noexcept;
	[[nodiscard]] const std::vector<SpotLightSource> &GetSpotLights() const noexcept;
	[[nodiscard]] const std::vector<DirectionalLightSource> &GetDirectionalLights() const noexcept;
	[[nodiscard]] std::span<const pipeline::render::GPULightRecord> GetGPURecords() const noexcept;
	[[nodiscard]] static float32 CalculateInfluenceRange(const PointLightSource &Light);
	[[nodiscard]] static float32 CalculateInfluenceRange(const SpotLightSource &Light);
	void Clear();
	void UploadLightSources(const std::vector<PointLightSource> &LightSources);
	void UploadLightSources(const std::vector<SpotLightSource> &LightSources);
	void UploadLightSources(const std::vector<DirectionalLightSource> &LightSources);
	void UploadSceneLights(const SceneCollection &Scene);

	LightBufferManager(const LightBufferManager &) = delete;
	LightBufferManager &operator=(const LightBufferManager &) = delete;
};
} // namespace pipeline::lighting
