#pragma once
#include "src/scene/PointLightSource.h"
#include "src/scene/SpotLightSource.h"
#include "src/scene/DirectionalLightSource.h"
#include "ShaderStorageBufferObject.h"
#include "RendererGpuTypes.h"
#include <vector>

class SceneCollection;

//4.3K fps avg before light refactor
class LightBufferManager final
{
private:
	uint32 maxLights = 0;
	GLuint unifiedLightBuffer = 0;
	ShaderSorageBufferObject<PointLightSource, BindingPoint::POINT_LIGHT_SOURCE> pointLightSourcesSSBO;
	ShaderSorageBufferObject<SpotLightSource, BindingPoint::SPOT_LIGHT_SOURCES> spotLightSourcesSSBO;
	ShaderSorageBufferObject<DirectionalLightSource, BindingPoint::DIRECTIONAL_LIGHT_SOURCES> directionalLightSourcesSSBO;
	std::vector<PointLightSource> pointLightSources;
	std::vector<SpotLightSource> spotLightSources;
	std::vector<DirectionalLightSource> directionalLightSources;
	void uploadUnifiedLightBuffer();

public:
	LightBufferManager(size_t maxLights);
	~LightBufferManager();
	LightBufferManager() = delete;

	uint32 getTotalLightSourceCount() const;
	[[nodiscard]] const std::vector<PointLightSource>& getPointLights() const noexcept;
	[[nodiscard]] const std::vector<SpotLightSource>& getSpotLights() const noexcept;
	[[nodiscard]] const std::vector<DirectionalLightSource>& getDirectionalLights() const noexcept;
	void clear();
	void uploadLightSources(std::vector<PointLightSource>& lightSources);
	void uploadLightSources(std::vector<SpotLightSource>& lightSources);
	void uploadLightSources(std::vector<DirectionalLightSource>& lightSources);
	void uploadSceneLights(const SceneCollection& scene);

	LightBufferManager(const LightBufferManager&) = delete;
	LightBufferManager& operator=(const LightBufferManager&) = delete;
};
