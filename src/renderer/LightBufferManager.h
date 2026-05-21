#pragma once
#include "src/scene/PointLightSource.h"
#include "src/scene/SpotLightSource.h"
#include "src/scene/DirectionalLightSource.h"
#include "ShaderStorageBufferObject.h"
#include "ShaderProgram.h"
#include <vector>

//4.3K fps avg before light refactor
class LightBufferManager final
{
private:
	ShaderProgram* shaderProgram;
	ShaderSorageBufferObject<PointLightSource, BindingPoint::POINT_LIGHT_SOURCE> pointLightSourcesSSBO;
	ShaderSorageBufferObject<SpotLightSource, BindingPoint::SPOT_LIGHT_SOURCES> spotLightSourcesSSBO;
	ShaderSorageBufferObject<DirectionalLightSource, BindingPoint::DIRECTIONAL_LIGHT_SOURCES> directionalLightSourcesSSBO;
	std::vector<PointLightSource> pointLightSources;
	std::vector<SpotLightSource> spotLightSources;
	std::vector<DirectionalLightSource> directionalLightSources;

public:
	LightBufferManager(size_t maxLights);
	LightBufferManager() = delete;

	int getTotalLightSourceCount() const;
	void clear();
	void uploadLightSources(std::vector<PointLightSource>& lightSources, ShaderProgram& shaderProgram);
	void uploadLightSources(std::vector<SpotLightSource>& lightSources, ShaderProgram& shaderProgram);
	void uploadLightSources(std::vector<DirectionalLightSource>& lightSources, ShaderProgram& shaderProgram);

	LightBufferManager(const LightBufferManager&) = delete;
	LightBufferManager& operator=(const LightBufferManager&) = delete;
};