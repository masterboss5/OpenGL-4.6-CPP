#pragma once

#include <vector>

#include "src/renderer/RenderCommand.h"
#include "src/scene/DirectionalLightSource.h"
#include "src/scene/PointLightSource.h"
#include "src/scene/SpotLightSource.h"

class StaticMeshObject;

// Per-frame renderer input.  This is intentionally not a world/ECS container:
// callers submit the renderable state they want drawn for the current frame.
class SceneCollection final
{
public:
	void beginFrame(uint64 frameNumber);
	void submit(const StaticMeshObject& object, uint32 objectID = 0, uint32 layerMask = ~uint32 { 0 }, uint64 revision = 0, bool transparent = false);
	void submit(renderer::RenderItem item);
	void addDirectionalLight(const DirectionalLightSource& light);
	void addPointLight(const PointLightSource& light);
	void addSpotLight(const SpotLightSource& light);
	void seal();
	void clear();

	[[nodiscard]] uint64 getFrameNumber() const noexcept;
	[[nodiscard]] bool isSealed() const noexcept;
	[[nodiscard]] const std::vector<renderer::RenderItem>& getRenderItems() const noexcept;
	[[nodiscard]] const std::vector<DirectionalLightSource>& getDirectionalLights() const noexcept;
	[[nodiscard]] const std::vector<PointLightSource>& getPointLights() const noexcept;
	[[nodiscard]] const std::vector<SpotLightSource>& getSpotLights() const noexcept;

private:
	uint64 frameNumber = 0;
	bool sealed = false;
	std::vector<renderer::RenderItem> renderItems;
	std::vector<DirectionalLightSource> directionalLights;
	std::vector<PointLightSource> pointLights;
	std::vector<SpotLightSource> spotLights;
};
