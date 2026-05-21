#pragma once
#include "src/scene/StaticMeshObject.h"
#include "src/scene/DirectionalLightSource.h"
#include "src/scene/PointLightSource.h"
#include "src/scene/SpotLightSource.h"

struct SceneCollection final
{
	std::vector<StaticMeshObject> staticMeshObjects;
	std::vector<DirectionalLightSource> directionalLightSources;
	std::vector<PointLightSource> pointLightSources;
	std::vector<SpotLightSource> spotLightSources;
};