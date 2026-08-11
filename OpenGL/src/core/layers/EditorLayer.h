#pragma once

#include "src/core/events/EventDispatcher.h"
#include "src/core/layers/ApplicationLayer.h"
#include "src/core/threading/RenderThread.h"
#include "src/core/threading/TaskScheduler.h"
#include "src/editor/EditorSession.h"
#include "src/editor/action/EditorActionRegistry.h"
#include "src/editor/ui/EditorUserInterface.h"
#include "src/editor/viewport/EditorCameraController.h"
#include "src/pipeline/render/RenderPipelineLibrary.h"
#include "src/pipeline/render/Renderer.h"
#include "src/resource/asset/AssetManager.h"
#include "src/scene/Camera.h"

#include <filesystem>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace core
{
class Window;
}

namespace pipeline::device
{
class Device;
}

namespace editor
{
struct EditorLayerSpecification final
{
	project::ProjectDescriptor Project;
	std::filesystem::path EngineContentRoot;
	pipeline::render::RenderViewID View{.Value = 2};
	uint64 MaximumRenderedFrames = 0;
	std::vector<cook::RuntimePackageFile> RuntimePackageFiles;
	std::optional<build::GameModuleBuildSpecification> GameModuleBuild;
};

class EditorLayer final : public ApplicationLayer
{
  public:
	EditorLayer(core::Window *Window, pipeline::device::Device &Device, EditorLayerSpecification Specification);
	~EditorLayer() override;

	EditorLayer(const EditorLayer &) = delete;
	EditorLayer &operator=(const EditorLayer &) = delete;
	EditorLayer(EditorLayer &&) = delete;
	EditorLayer &operator=(EditorLayer &&) = delete;

	void Run(const core::ApplicationFrame &Frame) override;

	[[nodiscard]] EditorSession &GetSession() noexcept;
	[[nodiscard]] const pipeline::render::RenderViewOutput &GetLastViewportOutput() const noexcept;

  private:
	struct EditorViewportInstance final
	{
		pipeline::render::RenderViewID View;
		string Name;
		std::unique_ptr<Camera> Camera;
		viewport::EditorCameraController CameraController;
		std::unique_ptr<viewport::EditorViewportRenderer> Renderer;
		pipeline::render::RenderViewOutput Output;
		bool Primary = false;
		bool Closing = false;
		bool RelativePointerActive = false;
		core::WindowID RelativePointerWindow;
	};

	struct RenderedViewportFrame final
	{
		pipeline::render::RenderViewID View;
		viewport::EditorViewportFrame Frame;
	};

	struct ViewportRenderRequest final
	{
		EditorViewportInstance *Viewport = nullptr;
		core::WindowExtent Extent;
		Camera Camera;
		std::vector<viewport::ViewportPickRequest> PickRequests;
		pipeline::render::ViewportSettings Settings;
		std::optional<pipeline::render::TransformGizmoOverlay> Gizmo;
	};

	struct EditorRenderFrame final
	{
		std::vector<RenderedViewportFrame> Viewports;
		std::vector<pipeline::render::RenderViewID> ReleasedViewports;
	};

	struct EditorFrameCompletion final
	{
		void Begin();
		void Complete(EditorRenderFrame Result);
		void Fail(std::exception_ptr Failure);
		[[nodiscard]] bool IsActive() const;
		[[nodiscard]] bool TryTake(EditorRenderFrame &Result, std::exception_ptr &Failure);
		[[nodiscard]] bool WaitAndTake(EditorRenderFrame &Result, std::exception_ptr &Failure);

		mutable std::mutex Mutex;
		std::condition_variable Condition;
		EditorRenderFrame Result;
		std::exception_ptr Failure;
		bool Active = false;
		bool Ready = false;
	};

	struct EditorRenderWorkPacket final
	{
		ui::EditorUIFrame InterfaceFrame;
		std::vector<ViewportRenderRequest> Requests;
		usize ActiveRequestCount = 0;
		std::vector<world::ObjectHandle> SelectedObjects;
		std::vector<EditorViewportInstance *> ClosingViewports;
	};

	struct PendingCameraInput final
	{
		float64 LastMouseX = 0.0;
		float64 LastMouseY = 0.0;
		float64 LeftPressX = 0.0;
		float64 LeftPressY = 0.0;
		float64 MouseDeltaX = 0.0;
		float64 MouseDeltaY = 0.0;
		float64 ScrollY = 0.0;
		core::input::Modifier LeftPressModifiers = core::input::Modifier::None;
		bool HasMousePosition = false;
		bool HasLeftPressPosition = false;
		bool LeftMouseDown = false;
		bool LeftMousePressed = false;
		bool LeftMouseReleased = false;
		bool MiddleMouseDown = false;
		bool RightMouseDown = false;
		bool AltDown = false;
		bool EscapePressed = false;
	};

	struct BufferedEditorPointerInput final
	{
		viewport::EditorCameraPointerInput Camera;
		float64 LeftPressX = 0.0;
		float64 LeftPressY = 0.0;
		core::input::Modifier LeftPressModifiers = core::input::Modifier::None;
		bool HasLeftPressPosition = false;
		bool LeftMouseDown = false;
		bool LeftMousePressed = false;
		bool LeftMouseReleased = false;
		bool EscapePressed = false;
	};

	void StartRenderThread(core::ApplicationServices &Services);
	[[nodiscard]] bool RetireCompletedFrame();
	void QueueViewportRequests(const ui::EditorUIFrame &InterfaceFrame);
	void ApplyViewportRequests();
	void ProcessEditorInput(const core::ApplicationFrame &Frame, const core::input::InputSnapshot &Input,
							const ui::EditorUIFrame &InterfaceFrame, action::EditorActionContext &ActionContext, float32 DeltaSeconds);
	void AccumulateCameraInput(const core::input::InputEvent &Event);
	[[nodiscard]] BufferedEditorPointerInput ConsumeEditorPointerInput(core::WindowID Window);
	void SubmitFrame(ui::EditorUIFrame InterfaceFrame, core::threading::TaskScheduler &Scheduler);
	void DestroyRenderResources();
	[[nodiscard]] EditorViewportInstance &CreateViewport(pipeline::render::RenderViewID View, bool Primary);
	[[nodiscard]] EditorViewportInstance *FindViewport(pipeline::render::RenderViewID View) noexcept;
	[[nodiscard]] std::span<const ui::EditorViewportPresentation> BuildViewportPresentations() const;

	core::Window *Window = nullptr;
	pipeline::device::Device *Device = nullptr;
	std::unique_ptr<EditorSession> Session;
	std::unique_ptr<resource::AssetManager> EngineAssets;
	action::EditorActionRegistry Actions;
	std::unique_ptr<ui::EditorUserInterface> UserInterface;
	std::unique_ptr<pipeline::render::Renderer> Renderer;
	std::unique_ptr<pipeline::render::RenderPipelineLibrary> Pipelines;
	std::vector<std::unique_ptr<EditorViewportInstance>> Viewports;
	core::threading::RenderThread *ExecutionThread = nullptr;
	std::shared_ptr<EditorFrameCompletion> PendingFrame;
	EditorRenderFrame RenderFrameScratch;
	// One packet is in flight at a time. Reusing it preserves request and
	// selection capacities across frames without sharing mutable state with the
	// render thread after completion.
	std::shared_ptr<EditorRenderWorkPacket> PendingRenderWork;
	// The owner and render threads have one outstanding frame at a time. Keep
	// the immutable CPU snapshot storage alive between submissions so worker
	// extraction reuses its vectors instead of rebuilding their capacities on
	// every editor frame.
	std::shared_ptr<pipeline::render::SceneRenderSnapshot> PendingSceneSnapshot;
	std::shared_ptr<pipeline::render::SceneRenderSnapshotBuildScratch> PendingSceneSnapshotScratch;
	std::vector<pipeline::render::RenderViewID> QueuedViewportClosures;
	std::vector<pipeline::render::RenderViewID> QueuedViewportProjectionToggles;
	mutable std::vector<ui::EditorViewportPresentation> OwnerViewportPresentations;
	std::vector<ui::EditorViewportPresentation> RenderViewportPresentations;
	std::vector<action::EditorActionResult> ActionResultsScratch;
	std::unordered_map<core::WindowID, PendingCameraInput> PendingCameraInputs;
	core::EventSubscription CameraInputSubscription;
	float64 PendingEditorDeltaSeconds = 0.0;
	uint32 QueuedViewportCreations = 0;
	uint64 NextRenderView = 0;
	uint64 MaximumRenderedFrames = 0;
	uint64 RenderedFrames = 0;
	bool ActionsInstalled = false;
	bool InterfaceRendererInitialized = false;
};
} // namespace editor
