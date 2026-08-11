#pragma once

#include "src/core/EngineAPI.h"

#include "src/pipeline/lighting/LightBufferManager.h"
#include "src/concepts.h"
#include "src/core/window/Window.h"
#include "src/pipeline/device/Device.h"
#include "src/pipeline/shader/GraphicsPipeline.h"
#include "src/pipeline/frame/FrameResourceRing.h"
#include "src/pipeline/graph/HybridDeferredFrameGraph.h"
#include "src/pipeline/mesh/MeshGpuResource.h"
#include "src/pipeline/render/RenderPassPipelineSet.h"
#include "src/pipeline/render/RenderData.h"
#include "src/pipeline/render/PickTable.h"
#include "src/pipeline/render/SelectionMask.h"
#include "src/pipeline/render/ViewportOverlay.h"
#include "src/pipeline/render/SceneExtractor.h"
#include "src/pipeline/render/ScenePreparation.h"
#include "src/pipeline/render/SceneRenderSnapshot.h"
#include "src/scene/SceneCollection.h"

#include <glm.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <thread>
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

namespace pipeline::render
{
struct RenderViewID final
{
	uint64 Value = 1;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Value != 0;
	}

	[[nodiscard]] bool operator==(const RenderViewID &) const noexcept = default;
};

struct RenderViewDescriptor final
{
	RenderViewID View;
	core::WindowExtent Extent;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->View.IsValid() && this->Extent.IsValid();
	}
};

struct RenderViewState final
{
	RenderViewID View;
	core::WindowExtent Extent;
	ViewportSettings Settings;
	uint64 Generation = 1;
	uint64 FrameNumber = 0;
	uint64 LastOutputFrameNumber = 0;
	bool HierarchicalDepthHistoryValid = false;
	bool TemporalHistoryValid = false;
	bool ExposureHistoryValid = false;
	bool HasPreviousCameraState = false;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->View.IsValid() && this->Extent.IsValid() && this->Generation != 0;
	}
};

struct RenderViewOutput final
{
	struct Statistics final
	{
		uint32 SubmittedObjects = 0;
		uint32 CandidateInstances = 0;
		uint32 RenderBatches = 0;
		uint32 DrawCalls = 0;
		uint32 DebugLines = 0;
		uint32 UnshadowedDirectionalLightRequests = 0;
		uint32 UnshadowedPointLightRequests = 0;
		uint32 UnshadowedSpotLightRequests = 0;
	};

	RenderViewID View;
	core::WindowExtent Extent;
	pipeline::graph::ExportedTexture Color;
	pipeline::graph::ExportedTexture ObjectID;
	pipeline::graph::ExportedTexture Depth;
	uint64 Generation = 0;
	uint64 FrameNumber = 0;
	// Ordinary view outputs do not retain a pick-table slot. A table is
	// attached only when the frame submitted asynchronous picking pixels.
	std::shared_ptr<const FramePickTable> PickTable;
	Statistics RenderStatistics;
	std::shared_ptr<const pipeline::graph::RenderGraphInspection> GraphInspection;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->View.IsValid() && this->Extent.IsValid() && this->Generation != 0 && this->Color.IsValid() &&
			   this->ObjectID.IsValid() && this->Color.ViewIdentity == this->View.Value &&
			   this->ObjectID.ViewIdentity == this->View.Value && this->Depth.ViewIdentity == this->View.Value &&
			   this->Color.ViewGeneration == this->Generation && this->ObjectID.ViewGeneration == this->Generation &&
			   this->Depth.ViewGeneration == this->Generation && this->Depth.IsValid() && this->Color.Extent.Width == this->Extent.Width &&
			   this->Color.Extent.Height == this->Extent.Height && this->ObjectID.Extent.Width == this->Extent.Width &&
			   this->ObjectID.Extent.Height == this->Extent.Height && this->Depth.Extent.Width == this->Extent.Width &&
			   this->Depth.Extent.Height == this->Extent.Height &&
			   (this->PickTable == nullptr || this->PickTable->GetFrameNumber() == this->FrameNumber) && this->GraphInspection != nullptr &&
			   this->Color.Format == pipeline::graph::TextureFormat::RGBA8SRGB &&
			   this->ObjectID.Format == pipeline::graph::TextureFormat::R32UnsignedInteger &&
			   this->Depth.Format == pipeline::graph::TextureFormat::Depth32Float && this->Color.FrameSerial != 0 &&
			   this->Color.FrameSerial == this->ObjectID.FrameSerial && this->Color.FrameSerial == this->Depth.FrameSerial;
	}

	[[nodiscard]] std::optional<world::ObjectHandle> ResolvePick(const PickID Identifier) const noexcept
	{
		if (this->PickTable == nullptr)
			return std::nullopt;
		return this->PickTable->Resolve(Identifier);
	}
};

struct PresentationRequest final
{
	RenderViewOutput Output;
	core::Window *Window = nullptr;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Output.IsValid() && this->Window != nullptr;
	}
};

class ENGINE_API Renderer final
{
  public:
	explicit Renderer(pipeline::device::Device &Device, bool HeadlessPresentationValidation = false,
					  std::filesystem::path HeadlessPresentationCapturePath = {});
	~Renderer();
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;
	Renderer(Renderer &&) = delete;
	Renderer &operator=(Renderer &&) = delete;

	template <IsLightSource LightType> void UploadLightSources(std::vector<LightType> &LightSources)
	{
		this->RequireOwnerThread();
		this->LightBufferManager.UploadLightSources(LightSources);
	}

	[[nodiscard]] uint32 GetDrawCount() const noexcept;
	[[nodiscard]] uint32 GetObjectsDrawn() const noexcept;
	void Render(const world::Scene &Scene, resource::AssetManager &Assets, const Camera &Camera, RenderViewID View = {},
				std::span<const world::ObjectHandle> SelectedObjects = {}, std::optional<TransformGizmoOverlay> GizmoOverlay = std::nullopt,
				ViewportSettings Settings = {}, std::span<const ViewportPickPixel> PickPixels = {});
	void Render(const SceneRenderSnapshot &Snapshot, resource::AssetManager &Assets, const Camera &Camera, RenderViewID View = {},
				std::span<const world::ObjectHandle> SelectedObjects = {}, std::optional<TransformGizmoOverlay> GizmoOverlay = std::nullopt,
				ViewportSettings Settings = {}, std::span<const ViewportPickPixel> PickPixels = {});
	[[nodiscard]] RenderViewOutput RenderView(const pipeline::render::RenderPassPipelineSet &Pipelines, const Camera &Camera,
											  core::WindowExtent Extent, RenderViewID View);
	[[nodiscard]] RenderViewOutput RenderView(const pipeline::render::RenderPassPipelineSet &Pipelines, const Camera &Camera,
											  const RenderViewDescriptor &Descriptor);
	void PresentView(const RenderViewOutput &Output, core::Window &Window);
	void PresentView(const PresentationRequest &Request);
	void PrepareInterfacePresentation(core::Window &Window);
	[[nodiscard]] std::optional<RenderViewState> GetViewState(RenderViewID View) const;
	void InvalidateView(RenderViewID View);
	void ReleaseView(RenderViewID View);
	void SetBackgroundColor(const glm::vec3 &Color);
	[[nodiscard]] const glm::vec3 &GetBackgroundColor() const noexcept;
	void SetSelectionOutline(const glm::vec4 &Color, uint32 Radius);
	[[nodiscard]] const glm::vec4 &GetSelectionOutlineColor() const noexcept;
	[[nodiscard]] uint32 GetSelectionOutlineRadius() const noexcept;
	[[nodiscard]] resource::GPURealizationBatchResult GetLastGPURealizationBatch() const noexcept;
	void EnableCulling() const;
	void DisableCulling() const;

  private:
	pipeline::device::DeviceHandle Device;
	std::thread::id OwnerThread;
	struct ShadowLayerCacheEntry final
	{
		GLuint Texture = 0;
		uint32 Layer = 0;
		pipeline::graph::Extent2D Extent;
		uint64 Signature = 0;
		bool FramebufferValidated = false;
		bool Valid = false;
	};
	struct PresentationFramebufferValidation final
	{
		GLuint Texture = 0;
		RenderViewID View;
		uint64 Generation = 0;
		pipeline::graph::Extent2D Extent;
		bool Valid = false;
	};
	GLuint ShadowFramebuffer = 0;
	GLuint PresentationFramebuffer = 0;
	PresentationFramebufferValidation PresentationFramebufferState;
	GLuint FullscreenVertexArray = 0;
	uint32 DrawCount = 0;
	uint32 ObjectsDrawn = 0;
	uint64 FrameNumber = 0;
	glm::vec3 BackgroundColor{0.025f, 0.035f, 0.055f};
	glm::vec4 SelectionOutlineColor{1.0f, 0.55f, 0.08f, 1.0f};
	uint32 SelectionOutlineRadius = 2;
	bool CollectingFrame = false;
	bool HeadlessPresentationValidation = false;
	bool HeadlessValidationFrameEligible = false;
	bool PresentationValidated = false;
	std::filesystem::path HeadlessPresentationCapturePath;
	resource::GPURealizationBatchResult LastGPURealizationBatch;
	std::unordered_map<uint32, ShadowLayerCacheEntry> ShadowLayerCache;
	static constexpr usize GraphInspectionCacheSize = 4;
	struct ViewState final
	{
		core::WindowExtent Extent{};
		ViewportSettings Settings;
		uint64 Generation = 1;
		uint64 FrameNumber = 0;
		uint64 LastOutputFrameNumber = 0;
		glm::mat4 PreviousViewProjection{1.0f};
		pipeline::graph::Extent2D HierarchicalDepthHistoryExtent{};
		bool HierarchicalDepthHistoryValid = false;
		pipeline::graph::Extent2D TemporalHistoryExtent{};
		bool TemporalHistoryValid = false;
		bool ExposureHistoryValid = false;
		glm::vec3 PreviousCameraPosition{0.0f};
		glm::vec3 PreviousCameraFront{0.0f, 0.0f, -1.0f};
		glm::vec3 PreviousCameraUp{0.0f, 1.0f, 0.0f};
		glm::mat4 PreviousProjection{1.0f};
		std::chrono::steady_clock::time_point LastFrameTime{};
		float32 FrameDeltaSeconds = 1.0f / 60.0f;
		bool HasPreviousCameraState = false;
		uint64 RenderTransformGeneration = 0;
		uint64 PreviousRenderTransformGeneration = 0;
		pipeline::render::RenderTransformHistory PreviousRenderTransforms;
		pipeline::render::RenderTransformHistory CurrentRenderTransforms;
		std::unique_ptr<pipeline::graph::RenderGraph> Graph;
		pipeline::graph::HybridDeferredFrameResources HybridResources;
		std::array<std::shared_ptr<pipeline::graph::RenderGraphInspection>, GraphInspectionCacheSize> GraphInspectionCache;
		string HistoryNamespace;
	};
	struct HybridPassExecutionState final
	{
		const pipeline::render::RenderPassPipelineSet *Pipelines = nullptr;
		pipeline::frame::FrameResources *Frame = nullptr;
		pipeline::render::RenderPreparationResult *Prepared = nullptr;
		pipeline::render::RenderPreparationResult *ShadowPrepared = nullptr;
		ViewState *View = nullptr;
		pipeline::graph::Extent2D Extent{};
		uint32 DirectionalCascadeCount = 0;
		uint32 SpotShadowCount = 0;
		uint32 PointShadowFaceCount = 0;
		uint64 ShadowCasterSignature = 0;
	};
	std::unordered_map<uint64, ViewState> Views;
	RenderViewID PendingView;
	pipeline::render::SelectionMask PendingSelectionMask;
	std::optional<pipeline::render::TransformGizmoOverlay> PendingGizmoOverlay;
	pipeline::render::ViewportSettings PendingViewportSettings;
	std::vector<pipeline::render::ViewportPickPixel> PendingPickPixels;
	std::vector<pipeline::render::GPUDebugLineRecord> PendingDebugLines;
	std::vector<pipeline::render::SceneDebugBounds> PendingDebugBounds;
	SceneCollection SceneCollection;
	// The scene-facing overload reuses this packet so extraction does not create
	// a temporary snapshot on the render submission path.
	pipeline::render::SceneRenderSnapshot SceneSnapshotScratch;
	pipeline::render::SceneRenderSnapshotBuildScratch SceneSnapshotBuildScratch;
	pipeline::mesh::MeshGPUCache MeshGPUCache;
	pipeline::render::SceneExtractorScratch SceneExtractorScratchStorage;
	pipeline::render::ScenePreparation ScenePreparation;
	pipeline::render::ScenePreparation::Workspace MainPreparationWorkspace;
	pipeline::render::ScenePreparation::Workspace ShadowPreparationWorkspace;
	pipeline::render::RenderPreparationResult Prepared;
	pipeline::render::RenderPreparationResult ShadowPrepared;
	struct ShadowDrawRange final
	{
		const pipeline::render::RenderBatch *Batch = nullptr;
		uint32 FirstInstance = 0;
		uint32 InstanceCount = 0;
	};
	std::vector<pipeline::render::PreparedInstance> ShadowVisibleInstances;
	std::vector<ShadowDrawRange> ShadowDrawRanges;
	std::vector<pipeline::render::GPUShadowRecord> ShadowRecords;
	pipeline::lighting::LightBufferManager LightBufferManager;
	std::unique_ptr<pipeline::frame::FrameResourceRing> FrameResources;
	std::vector<resource::AssetPtr<resource::Asset>> AssetPinTransfer;
	pipeline::graph::HybridDeferredFrameGraph HybridFrameGraph;
	HybridPassExecutionState ActiveHybridPass;
	pipeline::graph::HybridDeferredPassCallbacks HybridPassCallbacks;
	void RequireOwnerThread() const;
	void InitializeHybridPassCallbacks();
	static void DispatchHybridPass(void *UserData, pipeline::graph::HybridDeferredPassID Pass, pipeline::graph::RenderGraphContext &Context,
								   const pipeline::graph::HybridDeferredFrameResources &Resources);
	void ExecuteHybridPass(pipeline::graph::HybridDeferredPassID Pass, pipeline::graph::RenderGraphContext &Context,
						   const pipeline::graph::HybridDeferredFrameResources &Resources);
	void ExecuteHybridDispatch(const pipeline::shader::ComputePipeline &Pipeline, pipeline::graph::RenderGraphContext &Context,
							   pipeline::graph::TextureHandle Output);
	void ExecuteHybridDrawBatches(pipeline::graph::RenderGraphContext &Context, const pipeline::shader::GraphicsPipeline &Pipeline,
								  pipeline::render::RenderPassClass RequiredPass);
	void ExecuteHybridShadowLayers(GLuint Texture, uint32 FirstLayer, uint32 LayerCount, uint32 FirstRecord,
								   pipeline::graph::Extent2D ShadowExtent);
	void UploadFrameConstants(const Camera &Camera, core::WindowExtent Extent, pipeline::frame::FrameResources &Frame, ViewState &State);
	void ValidateHeadlessDepthCoverage(GLuint DepthTexture, pipeline::graph::Extent2D Extent) const;
	void ValidateHeadlessColorCoverage(GLuint ColorTexture, pipeline::graph::Extent2D Extent, string_view Stage) const;
	void ValidateHeadlessPresentation(core::Window &Window);
	[[nodiscard]] RenderViewOutput RenderViewInternal(const pipeline::render::RenderPassPipelineSet &Pipelines, const Camera &Camera,
													  core::WindowExtent Extent, RenderViewID View);
	void RecoverFailedFrame() noexcept;
};
} // namespace pipeline::render
