#pragma once

#include "LightBufferManager.h"
#include "src/concepts.h"
#include "src/core/window/Window.h"
#include "src/pipeline/device/Device.h"
#include "src/pipeline/shader/GraphicsPipeline.h"
#include "src/renderer/FrameResourceRing.h"
#include "src/renderer/HybridDeferredFrameGraph.h"
#include "src/renderer/MeshGpuResource.h"
#include "src/renderer/RenderPassPipelineSet.h"
#include "src/renderer/RendererGpuTypes.h"
#include "src/renderer/SceneExtractor.h"
#include "src/renderer/ScenePreparation.h"
#include "src/scene/SceneCollection.h"

#include <glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

class Camera;
namespace resource
{
class AssetManager;
}
namespace world
{
class Scene;
}

class OpenGLRenderer final
{
  public:
	explicit OpenGLRenderer(pipeline::device::Device &Device, bool HeadlessPresentationValidation = false);
	~OpenGLRenderer();
	OpenGLRenderer(const OpenGLRenderer &) = delete;
	OpenGLRenderer &operator=(const OpenGLRenderer &) = delete;
	OpenGLRenderer(OpenGLRenderer &&) = delete;
	OpenGLRenderer &operator=(OpenGLRenderer &&) = delete;

	template <IsLightSource LightType> void UploadLightSources(std::vector<LightType> &LightSources)
	{
		this->LightBufferManager.UploadLightSources(LightSources);
	}

	[[nodiscard]] uint32 GetDrawCount() const noexcept;
	[[nodiscard]] uint32 GetObjectsDrawn() const noexcept;
	void Render(const world::Scene &Scene, resource::AssetManager &Assets, const Camera &Camera);
	void RenderScene(const pipeline::shader::GraphicsPipeline &Pipeline, const Camera &Camera, core::Window &Window);
	void RenderScene(const renderer::RenderPassPipelineSet &Pipelines, const Camera &Camera, core::Window &Window);
	void SetBackgroundColor(const glm::vec3 &Color);
	[[nodiscard]] const glm::vec3 &GetBackgroundColor() const noexcept;
	void EnableCulling() const;
	void DisableCulling() const;

  private:
	pipeline::device::Device *Device = nullptr;
	struct ShadowLayerCacheEntry final
	{
		GLuint Texture = 0;
		uint32 Layer = 0;
		uint64 Signature = 0;
		bool Valid = false;
	};
	GLuint ShadowFramebuffer = 0;
	uint32 DrawCount = 0;
	uint32 ObjectsDrawn = 0;
	uint64 FrameNumber = 0;
	glm::mat4 PreviousViewProjection{1.0f};
	glm::vec3 BackgroundColor{0.025f, 0.035f, 0.055f};
	renderer::graph::Extent2D HierarchicalDepthHistoryExtent{};
	bool HierarchicalDepthHistoryValid = false;
	renderer::graph::Extent2D TemporalHistoryExtent{};
	bool TemporalHistoryValid = false;
	bool ExposureHistoryValid = false;
	glm::vec3 PreviousCameraPosition{0.0f};
	glm::vec3 PreviousCameraFront{0.0f, 0.0f, -1.0f};
	bool HasPreviousCameraState = false;
	bool CollectingFrame = false;
	bool HeadlessPresentationValidation = false;
	bool PresentationValidated = false;
	std::unordered_map<uint32, ShadowLayerCacheEntry> ShadowLayerCache;
	renderer::RenderTransformHistory PreviousRenderTransforms;
	renderer::RenderTransformHistory CurrentRenderTransforms;
	SceneCollection SceneCollection;
	renderer::MeshGPUCache MeshGPUCache;
	renderer::ScenePreparation ScenePreparation;
	LightBufferManager LightBufferManager;
	std::unique_ptr<renderer::FrameResourceRing> FrameResources;
	renderer::graph::RenderGraph RenderGraph;
	renderer::HybridDeferredFrameGraph HybridFrameGraph;
	void UploadFrameConstants(const Camera &Camera, core::WindowExtent Extent, renderer::FrameResources &Frame);
	void ValidateHeadlessDepthCoverage(GLuint DepthTexture, renderer::graph::Extent2D Extent) const;
	void ValidateHeadlessColorCoverage(GLuint ColorTexture, renderer::graph::Extent2D Extent, string_view Stage) const;
	void ValidateHeadlessPresentation(core::Window &Window);
	void RecoverFailedFrame() noexcept;
};
