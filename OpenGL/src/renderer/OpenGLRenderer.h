#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <glm.hpp>

#include "src/pipeline/shader/GraphicsPipeline.h"
#include "src/renderer/FrameResourceRing.h"
#include "src/renderer/HybridDeferredFrameGraph.h"
#include "src/renderer/RenderPassPipelineSet.h"
#include "src/renderer/RendererGpuTypes.h"
#include "src/renderer/ScenePreparation.h"
#include "src/scene/SceneCollection.h"
#include "LightBufferManager.h"

class Camera;
class StaticMeshObject;
class Window;

class OpenGLRenderer final
{
public:
	OpenGLRenderer();
	~OpenGLRenderer();
	OpenGLRenderer(const OpenGLRenderer&) = delete;
	OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;
	OpenGLRenderer(OpenGLRenderer&&) = delete;
	OpenGLRenderer& operator=(OpenGLRenderer&&) = delete;

	template<typename LightType>
	void uploadLightSources(std::vector<LightType>& lightSources)
	{
		this->lightBufferManager.uploadLightSources(lightSources);
	}

	[[nodiscard]] uint32 getDrawCount() const noexcept;
	[[nodiscard]] uint32 getObjectsDrawn() const noexcept;
	void render(const StaticMeshObject& worldObject);
	void renderScene(const pipeline::shader::GraphicsPipeline& pipeline, const Camera& camera, const Window& window);
	void renderScene(const renderer::RenderPassPipelineSet& pipelines, const Camera& camera, const Window& window);
	void enableCulling() const;
	void disableCulling() const;

private:
	struct ShadowLayerCacheEntry final
	{
		GLuint texture = 0;
		uint32 layer = 0;
		uint64 signature = 0;
		bool valid = false;
	};
	GLuint frameConstantsUBO = 0;
	GLuint materialSSBO = 0;
	GLuint shadowDataSSBO = 0;
	GLuint shadowFramebuffer = 0;
	uint32 drawCount = 0;
	uint32 objectsDrawn = 0;
	uint64 frameNumber = 0;
	glm::mat4 previousViewProjection { 1.0f };
	renderer::graph::Extent2D hierarchicalDepthHistoryExtent {};
	bool hierarchicalDepthHistoryValid = false;
	renderer::graph::Extent2D temporalHistoryExtent {};
	bool temporalHistoryValid = false;
	bool exposureHistoryValid = false;
	glm::vec3 previousCameraPosition { 0.0f };
	glm::vec3 previousCameraFront { 0.0f, 0.0f, -1.0f };
	bool hasPreviousCameraState = false;
	bool collectingFrame = false;
	bool headlessPresentationValidation = false;
	bool presentationValidated = false;
	std::unordered_map<uint32, ShadowLayerCacheEntry> shadowLayerCache;
	SceneCollection sceneCollection;
	renderer::ScenePreparation scenePreparation;
	LightBufferManager lightBufferManager;
	std::unique_ptr<renderer::FrameResourceRing> frameResources;
	renderer::graph::RenderGraph renderGraph;
	renderer::HybridDeferredFrameGraph hybridFrameGraph;
	void uploadFrameConstants(const Camera& camera, const Window& window);
	void validateHeadlessDepthCoverage(GLuint depthTexture, renderer::graph::Extent2D extent) const;
	void validateHeadlessColorCoverage(GLuint colorTexture, renderer::graph::Extent2D extent, string_view stage) const;
	void validateHeadlessPresentation(const Window& window);
};
