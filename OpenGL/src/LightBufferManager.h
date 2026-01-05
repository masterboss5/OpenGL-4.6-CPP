#pragma once
#include <memory>
#include "PointLightSource.h"
#include "SpotLightSource.h"
#include "DirectionalLightSource.h"
#include "ShaderStorageBufferObject.h"

class LightBufferManager final
{
private:
	ShaderSorageBufferObject<PointLightSource, BindingPoint::POINT_LIGHT_SOURCE> pointLightSourcesSSBO;
	ShaderSorageBufferObject<SpotLightSource, BindingPoint::SPOT_LIGHT_SOURCES> spotLightSourcesSSBO;
	ShaderSorageBufferObject<DirectionalLightSource, BindingPoint::DIRECTIONAL_LIGHT_SOURCES> directionalLightSourcesSSBO;
public:
	LightBufferManager(size_t maxPointLights);

	LightBufferManager(const LightBufferManager&) = delete;
	LightBufferManager& operator=(const LightBufferManager&) = delete;
};