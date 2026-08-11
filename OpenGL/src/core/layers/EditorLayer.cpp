#include "EditorLayer.h"

#include "src/core/app/ApplicationServices.h"
#include "src/core/window/Window.h"
#ifdef CreateWindow
#undef CreateWindow
#endif
#include "src/core/window/WindowManager.h"
#include "src/pipeline/device/Device.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace editor
{
void EditorLayer::EditorFrameCompletion::Begin()
{
	std::scoped_lock Lock(this->Mutex);
	if (this->Active || this->Ready)
		throw std::logic_error("Editor frame completion already contains an unretired frame");
	this->Failure = nullptr;
	this->Active = true;
}

void EditorLayer::EditorFrameCompletion::Complete(EditorRenderFrame CompletedFrame)
{
	{
		std::scoped_lock Lock(this->Mutex);
		if (!this->Active || this->Ready)
			throw std::logic_error("Editor frame completion was completed outside its active submission");
		this->Result = std::move(CompletedFrame);
		this->Active = false;
		this->Ready = true;
	}
	this->Condition.notify_all();
}

void EditorLayer::EditorFrameCompletion::Fail(std::exception_ptr Failure)
{
	if (Failure == nullptr)
		Failure = std::make_exception_ptr(std::runtime_error("Editor frame completion failed without a diagnostic"));
	{
		std::scoped_lock Lock(this->Mutex);
		if (!this->Active || this->Ready)
			throw std::logic_error("Editor frame completion failed outside its active submission");
		this->Failure = std::move(Failure);
		this->Result = {};
		this->Active = false;
		this->Ready = true;
	}
	this->Condition.notify_all();
}

bool EditorLayer::EditorFrameCompletion::IsActive() const
{
	std::scoped_lock Lock(this->Mutex);
	return this->Active || this->Ready;
}

bool EditorLayer::EditorFrameCompletion::TryTake(EditorRenderFrame &CompletedFrame, std::exception_ptr &Failure)
{
	std::scoped_lock Lock(this->Mutex);
	if (!this->Ready)
		return false;
	CompletedFrame = std::move(this->Result);
	Failure = std::move(this->Failure);
	this->Failure = nullptr;
	this->Ready = false;
	return true;
}

bool EditorLayer::EditorFrameCompletion::WaitAndTake(EditorRenderFrame &CompletedFrame, std::exception_ptr &Failure)
{
	std::unique_lock Lock(this->Mutex);
	if (!this->Active && !this->Ready)
		return false;
	this->Condition.wait(Lock, [this]() { return this->Ready; });
	CompletedFrame = std::move(this->Result);
	Failure = std::move(this->Failure);
	this->Failure = nullptr;
	this->Ready = false;
	return true;
}

EditorLayer::EditorLayer(core::Window *Window, pipeline::device::Device &Device, EditorLayerSpecification Specification)
	: Window(Window), Device(&Device)
{
	if (Window == nullptr)
		throw std::invalid_argument("EditorLayer requires an application window");
	if (Specification.EngineContentRoot.empty())
		throw std::invalid_argument("EditorLayer requires an explicit engine-content root");
	if (!Specification.View.IsValid())
		throw std::invalid_argument("EditorLayer requires a valid render-view identity");
	this->MaximumRenderedFrames = Specification.MaximumRenderedFrames;
	action::RegisterCoreEditorActions(this->Actions);

	this->Session = std::make_unique<EditorSession>(std::move(Specification.Project));
	this->Session->ConfigureRuntimePackage(std::move(Specification.RuntimePackageFiles));
	if (Specification.GameModuleBuild.has_value())
		this->Session->ConfigureGameModuleBuild(std::move(*Specification.GameModuleBuild));
	this->UserInterface = std::make_unique<ui::EditorUserInterface>(*this->Window);
	this->EngineAssets = std::make_unique<resource::AssetManager>(std::move(Specification.EngineContentRoot));
	this->PendingFrame = std::make_shared<EditorFrameCompletion>();
	(void)this->CreateViewport(Specification.View, true);
	this->NextRenderView = Specification.View.Value + 1;
	if (this->NextRenderView == 0)
		throw std::overflow_error("Editor viewport identity range is exhausted");
	this->Window->SetCursorMode(core::CursorMode::Visible);
}

EditorLayer::~EditorLayer()
{
	try
	{
		const bool HasRelativePointer =
			std::ranges::any_of(this->Viewports, [](const auto &Viewport) { return Viewport->RelativePointerActive; });
		if (HasRelativePointer)
		{
			this->Window->SetCursorMode(core::CursorMode::Visible);
			for (const std::unique_ptr<EditorViewportInstance> &Viewport : this->Viewports)
				Viewport->RelativePointerActive = false;
		}
		if (this->PendingFrame != nullptr && this->PendingFrame->IsActive())
		{
			EditorRenderFrame Frame;
			std::exception_ptr Failure;
			if (!this->PendingFrame->WaitAndTake(Frame, Failure))
				throw std::logic_error("Editor frame completion disappeared during shutdown");
			if (Failure != nullptr)
				std::rethrow_exception(Failure);
			if (this->Session != nullptr)
			{
				for (const RenderedViewportFrame &Viewport : Frame.Viewports)
					this->Session->ApplyViewportFrame(Viewport.View, Viewport.Frame);
			}
		}
		if (this->Session != nullptr)
		{
			this->Session->WaitForBackgroundWork();
		}
		this->DestroyRenderResources();
	}
	catch (...)
	{
		std::terminate();
	}
}

void EditorLayer::Run(const core::ApplicationFrame &Frame)
{
	if (!std::isfinite(Frame.Timing.DeltaSeconds) || Frame.Timing.DeltaSeconds < 0.0)
		throw std::invalid_argument("EditorLayer requires a finite non-negative frame delta");
	constexpr float64 MaximumPendingEditorDeltaSeconds = 0.25;
	this->PendingEditorDeltaSeconds =
		std::min(this->PendingEditorDeltaSeconds + Frame.Timing.DeltaSeconds, MaximumPendingEditorDeltaSeconds);
	if (!this->CameraInputSubscription.IsSubscribed())
	{
		this->CameraInputSubscription =
			Frame.Services.GetWindowManager().SubscribeInputEvents(-10'000,
																   [this](const core::input::InputEvent &Event)
																   {
																	   this->UserInterface->QueueInputEvent(Event);
																	   this->AccumulateCameraInput(Event);
																	   return core::EventPropagation::Continue;
																   });
	}
	const preferences::EditorPreferences &Preferences = this->Session->GetPreferences();
	for (const std::unique_ptr<EditorViewportInstance> &Viewport : this->Viewports)
	{
		viewport::EditorCameraSettings CameraSettings = Viewport->CameraController.GetSettings();
		if (CameraSettings.FlySpeed != Preferences.CameraMoveSpeed || CameraSettings.LookSensitivity != Preferences.CameraLookSensitivity)
		{
			CameraSettings.FlySpeed = Preferences.CameraMoveSpeed;
			CameraSettings.LookSensitivity = Preferences.CameraLookSensitivity;
			Viewport->CameraController.SetSettings(CameraSettings);
		}
	}

	this->StartRenderThread(Frame.Services);
	this->UserInterface->AttachWindowManager(Frame.Services.GetWindowManager());
	if (!this->InterfaceRendererInitialized)
	{
		this->ExecutionThread
			->Submit(
				[this]()
				{
					this->Renderer = std::make_unique<pipeline::render::Renderer>(*this->Device);
					this->Pipelines = std::make_unique<pipeline::render::RenderPipelineLibrary>(*this->Device, *this->EngineAssets,
																								!this->Window->IsSRGBPresentationCapable());
					this->UserInterface->InitializeRenderer();
				})
			.get();
		this->InterfaceRendererInitialized = true;
	}
	if (!this->ActionsInstalled)
	{
		this->Actions.InstallInput(Frame.Services.GetInputSystem());
		this->ActionsInstalled = true;
	}
	const bool RetiredFrame = this->RetireCompletedFrame();
	if (RetiredFrame && this->MaximumRenderedFrames != 0 && this->RenderedFrames >= this->MaximumRenderedFrames)
	{
		Frame.Services.RequestStop();
		return;
	}
	// Dear ImGui requires UpdatePlatformWindows() between every pair of
	// NewFrame() calls.  A frame remains active while the render thread owns
	// its detached-window contexts, so do not begin another UI frame until the
	// previous one has been retired on this thread.
	if (this->PendingFrame->IsActive())
		return;
	this->Session->RequestHierarchyRefresh(Frame.Services.GetTaskScheduler());
	(void)this->Session->PollAssetImport(Frame.Services.GetTaskScheduler());
	if (this->Session->PollAssetReload(Frame.Services.GetTaskScheduler()))
	{
		const std::optional<asset::AssetReloadResult> &Result = this->Session->GetAssetReloadService().GetResult();
		if (Result.has_value())
		{
			Frame.Services.GetDiagnostics().Publish(Result->Succeeded ? core::diagnostics::DiagnosticSeverity::Information
																	  : core::diagnostics::DiagnosticSeverity::Error,
													"AssetReload", Result->Diagnostic);
		}
	}
	if (this->Session->PollAssetContentOperation(Frame.Services.GetTaskScheduler()))
	{
		const std::optional<asset::AssetContentResult> &Result = this->Session->GetAssetContentService().GetResult();
		if (Result.has_value())
		{
			Frame.Services.GetDiagnostics().Publish(Result->Committed ? core::diagnostics::DiagnosticSeverity::Information
																	  : core::diagnostics::DiagnosticSeverity::Error,
													"Content", Result->Diagnostic);
		}
	}
	if (this->Session->PollPrivateMaterialAssignments(Frame.Services.GetTaskScheduler()))
	{
		for (material::PrivateMaterialAssignmentResult &Result : this->Session->GetPrivateMaterialAssignmentService().TakeResults())
		{
			Frame.Services.GetDiagnostics().Publish(Result.Committed ? core::diagnostics::DiagnosticSeverity::Information
																	 : core::diagnostics::DiagnosticSeverity::Error,
													"Material", Result.Diagnostic);
		}
	}
	(void)this->Session->PollCookPackage();
	if (this->Session->PollProjectBuild(Frame.Services.GetTaskScheduler()))
	{
		const std::optional<build::ProjectBuildResult> &Build = this->Session->GetProjectBuildService().GetResult();
		if (Build.has_value())
		{
			const bool Succeeded = Build->State == build::ProjectBuildState::Succeeded;
			const string SuccessMessage = Build->RuntimeDirectory.empty() ? "GameModule build and publication completed"
																		  : "Project build passed validation; packaging started";
			Frame.Services.GetDiagnostics().Publish(Succeeded ? core::diagnostics::DiagnosticSeverity::Information
															  : core::diagnostics::DiagnosticSeverity::Error,
													"Build", Succeeded ? SuccessMessage : Build->Diagnostic);
		}
	}
	this->Session->TickRecovery(Frame.Services.GetTaskScheduler());
	if (this->Session->PollRecovery())
	{
		const std::optional<recovery::EditorRecoveryResult> &Recovery = this->Session->GetRecoveryStore().GetLastResult();
		if (Recovery.has_value())
		{
			const bool Succeeded = Recovery->Candidate.DocumentID.IsValid();
			Frame.Services.GetDiagnostics().Publish(Succeeded ? core::diagnostics::DiagnosticSeverity::Information
															  : core::diagnostics::DiagnosticSeverity::Error,
													"Recovery", Recovery->Diagnostic);
		}
	}
	this->Session->RequestContentRefresh(Frame.Services.GetTaskScheduler());
	(void)this->Session->PollHierarchyRefresh();
	(void)this->Session->PollContentRefresh(Frame.Services.GetTaskScheduler());
	if (Frame.FramebufferExtent.IsValid())
	{
		(void)this->Session->PollGameModule();
		this->Session->TickPlay(Frame.Services.GetTaskScheduler(), this->PendingEditorDeltaSeconds);
		action::EditorActionContext ActionContext{.Session = *this->Session,
												  .Scheduler = Frame.Services.GetTaskScheduler(),
												  .Diagnostics = Frame.Services.GetDiagnostics(),
												  .Window = this->Window};
		core::FrameTiming EditorTiming = Frame.Timing;
		EditorTiming.DeltaSeconds = this->PendingEditorDeltaSeconds;
		const core::ApplicationFrame EditorFrame(EditorTiming, Frame.Window, Frame.FramebufferExtent, Frame.Input, Frame.WindowEvents,
												 Frame.Services);
		const float32 EditorDeltaSeconds = static_cast<float32>(this->PendingEditorDeltaSeconds);
		this->PendingEditorDeltaSeconds = 0.0;
		const std::span<const ui::EditorViewportPresentation> ViewportPresentations = this->BuildViewportPresentations();
		ui::EditorUIFrame InterfaceFrame = this->UserInterface->BuildFrame(EditorFrame, *this->Session, this->Actions, ActionContext,
																		   Frame.Services.GetDiagnostics(), ViewportPresentations, true);
		this->QueueViewportRequests(InterfaceFrame);
		if (!this->PendingFrame->IsActive())
			this->ApplyViewportRequests();
		this->ProcessEditorInput(EditorFrame, Frame.Input, InterfaceFrame, ActionContext, EditorDeltaSeconds);
		if (!this->PendingFrame->IsActive())
			this->SubmitFrame(std::move(InterfaceFrame), Frame.Services.GetTaskScheduler());
	}
}

EditorSession &EditorLayer::GetSession() noexcept
{
	return *this->Session;
}

const pipeline::render::RenderViewOutput &EditorLayer::GetLastViewportOutput() const noexcept
{
	return this->Viewports.front()->Output;
}

void EditorLayer::StartRenderThread(core::ApplicationServices &Services)
{
	if (this->ExecutionThread != nullptr)
		return;
	pipeline::render::RenderPipelineLibrary::PreloadShaderSources(*this->EngineAssets, Services.GetTaskScheduler());
	this->ExecutionThread = &Services.GetRenderThread();
	this->ExecutionThread->Start(this->Window->GetContext());
}

bool EditorLayer::RetireCompletedFrame()
{
	if (this->PendingFrame == nullptr)
		return false;
	EditorRenderFrame Frame;
	std::exception_ptr Failure;
	if (!this->PendingFrame->TryTake(Frame, Failure))
		return false;
	if (Failure != nullptr)
		std::rethrow_exception(Failure);
	const auto RecycleViewportFrames = [this, &Frame]() noexcept
	{
		for (RenderedViewportFrame &ViewportFrame : Frame.Viewports)
		{
			EditorViewportInstance *Viewport = this->FindViewport(ViewportFrame.View);
			if (Viewport != nullptr && Viewport->Renderer != nullptr)
				Viewport->Renderer->RecycleFrame(std::move(ViewportFrame.Frame));
		}
		Frame.Viewports.clear();
	};
	try
	{
		for (RenderedViewportFrame &ViewportFrame : Frame.Viewports)
		{
			EditorViewportInstance *Viewport = this->FindViewport(ViewportFrame.View);
			if (Viewport == nullptr || Viewport->Closing)
				continue;
			Viewport->Output = ViewportFrame.Frame.Output;
			this->Session->ApplyViewportFrame(ViewportFrame.View, ViewportFrame.Frame);
		}
		RecycleViewportFrames();
	}
	catch (...)
	{
		RecycleViewportFrames();
		throw;
	}
	for (const pipeline::render::RenderViewID Released : Frame.ReleasedViewports)
	{
		this->Session->ReleaseViewport(Released);
		std::erase_if(this->Viewports, [Released](const auto &Viewport) { return Viewport->View == Released; });
	}
	this->ApplyViewportRequests();
	const bool FrameHasUsefulStorage = !Frame.Viewports.empty() || !Frame.ReleasedViewports.empty() ||
									   Frame.Viewports.capacity() > this->RenderFrameScratch.Viewports.capacity() ||
									   Frame.ReleasedViewports.capacity() > this->RenderFrameScratch.ReleasedViewports.capacity();
	if (FrameHasUsefulStorage)
		this->RenderFrameScratch = std::move(Frame);
	++this->RenderedFrames;
	return true;
}

void EditorLayer::QueueViewportRequests(const ui::EditorUIFrame &InterfaceFrame)
{
	this->QueuedViewportCreations += InterfaceFrame.CreateViewportRequestCount;
	for (const pipeline::render::RenderViewID View : InterfaceFrame.ToggleViewportProjectionRequests)
	{
		if (this->FindViewport(View) != nullptr)
			this->QueuedViewportProjectionToggles.push_back(View);
	}
	for (const pipeline::render::RenderViewID View : InterfaceFrame.CloseViewportRequests)
	{
		EditorViewportInstance *Viewport = this->FindViewport(View);
		if (Viewport == nullptr || Viewport->Primary || Viewport->Closing ||
			std::ranges::find(this->QueuedViewportClosures, View) != this->QueuedViewportClosures.end())
		{
			continue;
		}
		this->QueuedViewportClosures.push_back(View);
	}
}

void EditorLayer::ApplyViewportRequests()
{
	if (this->PendingFrame != nullptr && this->PendingFrame->IsActive())
		return;
	for (const pipeline::render::RenderViewID View : this->QueuedViewportClosures)
	{
		EditorViewportInstance *Viewport = this->FindViewport(View);
		if (Viewport != nullptr && !Viewport->Primary)
			Viewport->Closing = true;
	}
	this->QueuedViewportClosures.clear();
	for (const pipeline::render::RenderViewID View : this->QueuedViewportProjectionToggles)
	{
		EditorViewportInstance *Viewport = this->FindViewport(View);
		if (Viewport == nullptr || Viewport->Closing)
			continue;
		Viewport->Camera->Projection = Viewport->Camera->Projection == CameraProjectionMode::Perspective
										   ? CameraProjectionMode::Orthographic
										   : CameraProjectionMode::Perspective;
	}
	this->QueuedViewportProjectionToggles.clear();

	while (this->QueuedViewportCreations > 0)
	{
		const pipeline::render::RenderViewID View{.Value = this->NextRenderView};
		++this->NextRenderView;
		if (!View.IsValid() || this->NextRenderView == 0)
			throw std::overflow_error("Editor viewport identity range is exhausted");
		(void)this->CreateViewport(View, false);
		--this->QueuedViewportCreations;
	}
}

void EditorLayer::ProcessEditorInput(const core::ApplicationFrame &Frame, const core::input::InputSnapshot &Input,
									 const ui::EditorUIFrame &InterfaceFrame, action::EditorActionContext &ActionContext,
									 const float32 DeltaSeconds)
{
	const bool RelativePointerActive =
		std::ranges::any_of(this->Viewports, [](const auto &Viewport) { return Viewport->RelativePointerActive; });

	const ui::EditorViewportRegion *ViewportRegion = nullptr;
	EditorViewportInstance *Viewport = nullptr;
	for (const ui::EditorViewportRegion &Candidate : InterfaceFrame.Viewports)
	{
		EditorViewportInstance *Instance = this->FindViewport(Candidate.View);
		if (Instance == nullptr || Instance->Closing)
			continue;
		if (Instance->RelativePointerActive)
		{
			ViewportRegion = &Candidate;
			Viewport = Instance;
			break;
		}
		if (Candidate.Hovered || (ViewportRegion == nullptr && Candidate.Focused))
		{
			ViewportRegion = &Candidate;
			Viewport = Instance;
			if (Candidate.Hovered)
				break;
		}
	}
	core::Window *InteractionWindow = this->Window;
	const core::input::InputSnapshot *ActiveInput = &Input;
	if (ViewportRegion != nullptr && ViewportRegion->Window.IsValid())
	{
		core::Window *CandidateWindow = Frame.Services.GetWindowManager().FindManagedWindow(ViewportRegion->Window);
		if (CandidateWindow != nullptr)
		{
			InteractionWindow = CandidateWindow;
			ActiveInput = &Frame.Services.GetInputSystem().GetSnapshot(ViewportRegion->Window);
		}
	}
	const BufferedEditorPointerInput BufferedPointerInput = this->ConsumeEditorPointerInput(InteractionWindow->GetID());
	const bool ViewportMovementInput = ViewportRegion != nullptr &&
									   (ViewportRegion->Hovered || ViewportRegion->Focused || RelativePointerActive) &&
									   (ActiveInput->IsKeyDown(core::input::Key::W) || ActiveInput->IsKeyDown(core::input::Key::A) ||
										ActiveInput->IsKeyDown(core::input::Key::S) || ActiveInput->IsKeyDown(core::input::Key::D) ||
										ActiveInput->IsKeyDown(core::input::Key::Q) || ActiveInput->IsKeyDown(core::input::Key::E));
	this->Actions.ProcessInputInto(InteractionWindow->GetID(), ActionContext,
								   InterfaceFrame.WantsKeyboard || RelativePointerActive ||
									   ActiveInput->IsMouseButtonDown(core::input::MouseButton::Right) || ViewportMovementInput ||
									   this->Session->GetTransformGizmo().IsDragging(),
								   this->ActionResultsScratch);
	if (ViewportRegion == nullptr || Viewport == nullptr || !ViewportRegion->IsValid())
	{
		if (RelativePointerActive)
		{
			for (const std::unique_ptr<EditorViewportInstance> &Instance : this->Viewports)
			{
				Instance->RelativePointerActive = false;
				Instance->RelativePointerWindow = {};
			}
			InteractionWindow->SetCursorMode(core::CursorMode::Visible);
		}
		return;
	}
	const core::WindowPosition InteractionWindowPosition = InteractionWindow->GetPosition();
	const auto NormalizePointer = [&](const float64 X, const float64 Y)
	{
		return glm::vec2(std::clamp((static_cast<float32>(X) + static_cast<float32>(InteractionWindowPosition.X) - ViewportRegion->Left) /
										ViewportRegion->Width,
									0.0f, 1.0f),
						 std::clamp((static_cast<float32>(Y) + static_cast<float32>(InteractionWindowPosition.Y) - ViewportRegion->Top) /
										ViewportRegion->Height,
									0.0f, 1.0f));
	};
	const glm::vec2 Pointer = NormalizePointer(ActiveInput->GetMouseX(), ActiveInput->GetMouseY());
	const bool LeftMousePressed =
		BufferedPointerInput.LeftMousePressed || ActiveInput->GetMouseButton(core::input::MouseButton::Left).Pressed;
	const bool LeftMouseReleased =
		BufferedPointerInput.LeftMouseReleased || ActiveInput->GetMouseButton(core::input::MouseButton::Left).Released;
	const bool LeftMouseDown = BufferedPointerInput.LeftMouseDown || ActiveInput->IsMouseButtonDown(core::input::MouseButton::Left);
	const bool EscapePressed = BufferedPointerInput.EscapePressed || ActiveInput->WasKeyPressed(core::input::Key::Escape);
	const glm::vec2 PressPointer = BufferedPointerInput.HasLeftPressPosition
									   ? NormalizePointer(BufferedPointerInput.LeftPressX, BufferedPointerInput.LeftPressY)
									   : Pointer;
	const bool BufferedPressInsideViewport =
		BufferedPointerInput.LeftMousePressed && BufferedPointerInput.HasLeftPressPosition &&
		ViewportRegion->Contains(BufferedPointerInput.LeftPressX + static_cast<float64>(InteractionWindowPosition.X),
								 BufferedPointerInput.LeftPressY + static_cast<float64>(InteractionWindowPosition.Y));
	viewport::TransformGizmoController &Gizmo = this->Session->GetTransformGizmo();
	const bool EditWorldInteractive = this->Session->GetPlaySession().GetState() == play::PlaySessionState::Stopped;
	const bool ViewportPointerAvailable =
		ViewportRegion->Hovered || BufferedPressInsideViewport || Viewport->RelativePointerActive || Gizmo.IsDragging();
	bool GizmoConsumedPointer = false;

	if (EditWorldInteractive && Gizmo.IsDragging())
	{
		GizmoConsumedPointer = true;
		if (EscapePressed)
			Gizmo.CancelDrag();
		else if (LeftMouseReleased)
			Gizmo.CommitDrag();
		else if (LeftMouseDown)
			(void)Gizmo.UpdateDrag(*Viewport->Camera, ViewportRegion->PixelExtent, Pointer.x, Pointer.y);
	}
	else if (ViewportPointerAvailable && LeftMousePressed)
	{
		const core::input::Modifier PressModifiers =
			BufferedPointerInput.LeftMousePressed ? BufferedPointerInput.LeftPressModifiers : ActiveInput->GetModifiers();
		const bool Alt = core::input::Contains(PressModifiers, core::input::Modifier::Alt);
		if (!Alt)
		{
			const viewport::TransformGizmoHandle Handle = EditWorldInteractive
															  ? Gizmo.HitTest(this->Session->GetDocument(), *Viewport->Camera,
																			  ViewportRegion->PixelExtent, PressPointer.x, PressPointer.y)
															  : viewport::TransformGizmoHandle::None;
			if (Handle != viewport::TransformGizmoHandle::None)
			{
				GizmoConsumedPointer = Gizmo.BeginDrag(this->Session->GetDocument(), *Viewport->Camera, ViewportRegion->PixelExtent,
													   PressPointer.x, PressPointer.y, Handle);
				if (GizmoConsumedPointer && (LeftMouseDown || LeftMouseReleased))
					(void)Gizmo.UpdateDrag(*Viewport->Camera, ViewportRegion->PixelExtent, Pointer.x, Pointer.y);
				if (GizmoConsumedPointer && LeftMouseReleased)
					Gizmo.CommitDrag();
			}
			else
			{
				const bool Control = core::input::Contains(PressModifiers, core::input::Modifier::Control);
				const bool Shift = core::input::Contains(PressModifiers, core::input::Modifier::Shift);
				const viewport::SelectionOperation Operation =
					Control ? viewport::SelectionOperation::Toggle
							: (Shift ? viewport::SelectionOperation::Add : viewport::SelectionOperation::Replace);
				(void)this->Session->QueueViewportPick(Viewport->View, PressPointer.x, PressPointer.y, Operation);
			}
		}
	}

	if (EditWorldInteractive && !Gizmo.IsDragging() && ActiveInput->WasKeyPressed(core::input::Key::F))
		(void)Viewport->CameraController.FocusSelection(this->Session->GetDocument(), *Viewport->Camera);
	const viewport::EditorCameraInteraction CameraInteraction = Viewport->CameraController.Update(
		*Viewport->Camera, *ActiveInput, DeltaSeconds, BufferedPointerInput.Camera, ViewportPointerAvailable, ViewportRegion->Focused,
		InteractionWindow->IsFocused(), EscapePressed,
		GizmoConsumedPointer || Gizmo.IsDragging() || (InterfaceFrame.WantsPointer && !ViewportPointerAvailable),
		InterfaceFrame.WantsKeyboard && !ViewportRegion->Focused);
	if (CameraInteraction.WantsRelativePointer != Viewport->RelativePointerActive)
	{
		if (CameraInteraction.WantsRelativePointer)
		{
			for (const std::unique_ptr<EditorViewportInstance> &Instance : this->Viewports)
			{
				Instance->RelativePointerActive = false;
				Instance->RelativePointerWindow = {};
			}
		}
		InteractionWindow->SetCursorMode(CameraInteraction.WantsRelativePointer ? core::CursorMode::Relative : core::CursorMode::Visible);
		Viewport->RelativePointerActive = CameraInteraction.WantsRelativePointer;
		Viewport->RelativePointerWindow = CameraInteraction.WantsRelativePointer ? InteractionWindow->GetID() : core::WindowID{};
	}
}

void EditorLayer::AccumulateCameraInput(const core::input::InputEvent &Event)
{
	PendingCameraInput &Pending = this->PendingCameraInputs[Event.Window];
	if (Event.Type == core::input::InputEventType::Key)
	{
		Pending.AltDown = core::input::Contains(Event.Modifiers, core::input::Modifier::Alt);
		if (Event.Key == core::input::Key::Escape && Event.State == core::input::InputState::Pressed)
			Pending.EscapePressed = true;
	}
	else if (Event.Type == core::input::InputEventType::MouseButton)
	{
		const bool Down = Event.State != core::input::InputState::Released;
		if (Event.MouseButton == core::input::MouseButton::Left)
		{
			if (Down && !Pending.LeftMouseDown)
			{
				Pending.LeftMousePressed = true;
				Pending.LeftPressModifiers = Event.Modifiers;
				Pending.HasLeftPressPosition = Pending.HasMousePosition;
				Pending.LeftPressX = Pending.LastMouseX;
				Pending.LeftPressY = Pending.LastMouseY;
			}
			else if (!Down && Pending.LeftMouseDown)
			{
				Pending.LeftMouseReleased = true;
			}
			Pending.LeftMouseDown = Down;
		}
		else if (Event.MouseButton == core::input::MouseButton::Middle)
			Pending.MiddleMouseDown = Down;
		else if (Event.MouseButton == core::input::MouseButton::Right)
		{
			Pending.RightMouseDown = Down;
		}
		Pending.AltDown = core::input::Contains(Event.Modifiers, core::input::Modifier::Alt);
	}
	else if (Event.Type == core::input::InputEventType::MouseMove)
	{
		if (!std::isfinite(Event.X) || !std::isfinite(Event.Y))
		{
			Pending.HasMousePosition = false;
			Pending.MouseDeltaX = 0.0;
			Pending.MouseDeltaY = 0.0;
			return;
		}
		const bool RelativePointerActive =
			std::ranges::any_of(this->Viewports, [&Event](const auto &Viewport)
								{ return Viewport->RelativePointerActive && Viewport->RelativePointerWindow == Event.Window; });
		if (Pending.HasMousePosition &&
			(Pending.RightMouseDown || Pending.MiddleMouseDown || (Pending.LeftMouseDown && Pending.AltDown) || RelativePointerActive))
		{
			Pending.MouseDeltaX += Event.X - Pending.LastMouseX;
			Pending.MouseDeltaY += Event.Y - Pending.LastMouseY;
		}
		Pending.LastMouseX = Event.X;
		Pending.LastMouseY = Event.Y;
		Pending.HasMousePosition = true;
	}
	else if (Event.Type == core::input::InputEventType::MouseScroll)
	{
		if (std::isfinite(Event.Y))
			Pending.ScrollY += Event.Y;
	}
}

EditorLayer::BufferedEditorPointerInput EditorLayer::ConsumeEditorPointerInput(const core::WindowID Window)
{
	const auto Found = this->PendingCameraInputs.find(Window);
	if (Found == this->PendingCameraInputs.end())
		return {};
	PendingCameraInput &Pending = Found->second;
	const BufferedEditorPointerInput Result{.Camera = {.DeltaX = static_cast<float32>(Pending.MouseDeltaX),
													   .DeltaY = static_cast<float32>(Pending.MouseDeltaY),
													   .ScrollY = static_cast<float32>(Pending.ScrollY),
													   .RightMouseDown = Pending.RightMouseDown},
											.LeftPressX = Pending.LeftPressX,
											.LeftPressY = Pending.LeftPressY,
											.LeftPressModifiers = Pending.LeftPressModifiers,
											.HasLeftPressPosition = Pending.HasLeftPressPosition,
											.LeftMouseDown = Pending.LeftMouseDown,
											.LeftMousePressed = Pending.LeftMousePressed,
											.LeftMouseReleased = Pending.LeftMouseReleased,
											.EscapePressed = Pending.EscapePressed};
	Pending.MouseDeltaX = 0.0;
	Pending.MouseDeltaY = 0.0;
	Pending.ScrollY = 0.0;
	Pending.HasLeftPressPosition = false;
	Pending.LeftMousePressed = false;
	Pending.LeftMouseReleased = false;
	Pending.EscapePressed = false;
	return Result;
}

void EditorLayer::SubmitFrame(ui::EditorUIFrame InterfaceFrame, core::threading::TaskScheduler &Scheduler)
{
	if (this->PendingRenderWork == nullptr)
		this->PendingRenderWork = std::make_shared<EditorRenderWorkPacket>();
	const std::shared_ptr<EditorRenderWorkPacket> Work = this->PendingRenderWork;
	Work->InterfaceFrame = std::move(InterfaceFrame);
	for (ViewportRenderRequest &Request : Work->Requests)
	{
		Request.Viewport = nullptr;
		Request.PickRequests.clear();
		Request.Gizmo.reset();
	}
	Work->ActiveRequestCount = 0;
	if (Work->Requests.capacity() < Work->InterfaceFrame.Viewports.size())
		Work->Requests.reserve(Work->InterfaceFrame.Viewports.size());
	this->Session->ResolveViewportSelectionInto(Work->SelectedObjects);
	pipeline::render::SceneRenderSnapshotBuildOptions SnapshotOptions{.RespectEditorVisibility = true};
	for (const ui::EditorViewportRegion &Region : Work->InterfaceFrame.Viewports)
	{
		SnapshotOptions.IncludeBounds =
			SnapshotOptions.IncludeBounds || Region.Settings.Overlays.Bounds || Region.Settings.Overlays.Culling;
		SnapshotOptions.IncludeSkeletons = SnapshotOptions.IncludeSkeletons || Region.Settings.Overlays.Skeletons;
		SnapshotOptions.IncludeCameras = SnapshotOptions.IncludeCameras || Region.Settings.Overlays.Cameras;
		SnapshotOptions.IncludeLights = SnapshotOptions.IncludeLights || Region.Settings.Overlays.Lights;
	}
	world::Scene *ViewportScene = &this->Session->GetViewportScene();
	resource::AssetManager *ProjectAssets = &this->Session->GetProject().GetAssetManager();
	const bool ShowGizmo = this->Session->GetPlaySession().GetState() == play::PlaySessionState::Stopped;
	for (const ui::EditorViewportRegion &Region : Work->InterfaceFrame.Viewports)
	{
		EditorViewportInstance *Viewport = this->FindViewport(Region.View);
		if (Viewport == nullptr || Viewport->Closing || !Region.IsValid())
			continue;
		ViewportRenderRequest *Request = nullptr;
		if (Work->ActiveRequestCount < Work->Requests.size())
		{
			Request = &Work->Requests[Work->ActiveRequestCount];
			Request->Viewport = Viewport;
			Request->Extent = Region.PixelExtent;
			Request->Camera = *Viewport->Camera;
			Request->Settings = Region.Settings;
		}
		else
		{
			Request = &Work->Requests.emplace_back(ViewportRenderRequest{
				.Viewport = Viewport, .Extent = Region.PixelExtent, .Camera = *Viewport->Camera, .Settings = Region.Settings});
		}
		++Work->ActiveRequestCount;
		this->Session->CollectViewportPickRequestsInto(Viewport->View, Region.PixelExtent, Request->PickRequests);
		const viewport::TransformGizmoVisualState GizmoState =
			ShowGizmo ? this->Session->GetTransformGizmo().BuildVisualState(this->Session->GetDocument(), Request->Camera)
					  : viewport::TransformGizmoVisualState{};
		if (GizmoState.Visible)
		{
			Request->Gizmo = pipeline::render::TransformGizmoOverlay{.Visible = true,
																	 .Pivot = GizmoState.Pivot,
																	 .Basis = GizmoState.Basis,
																	 .WorldScale = GizmoState.WorldScale,
																	 .Operation = static_cast<uint32>(GizmoState.Operation),
																	 .ActiveHandle = static_cast<uint32>(GizmoState.ActiveHandle)};
		}
	}
	Work->ClosingViewports.clear();
	if (Work->ClosingViewports.capacity() < this->Viewports.size())
		Work->ClosingViewports.reserve(this->Viewports.size());
	for (const std::unique_ptr<EditorViewportInstance> &Viewport : this->Viewports)
	{
		if (Viewport->Closing)
			Work->ClosingViewports.push_back(Viewport.get());
	}
	if (this->PendingSceneSnapshot == nullptr)
		this->PendingSceneSnapshot = std::make_shared<pipeline::render::SceneRenderSnapshot>();
	if (this->PendingSceneSnapshotScratch == nullptr)
		this->PendingSceneSnapshotScratch = std::make_shared<pipeline::render::SceneRenderSnapshotBuildScratch>();
	const std::shared_ptr<pipeline::render::SceneRenderSnapshot> Snapshot = this->PendingSceneSnapshot;
	const std::shared_ptr<pipeline::render::SceneRenderSnapshotBuildScratch> SnapshotScratch = this->PendingSceneSnapshotScratch;
	const std::shared_ptr<EditorFrameCompletion> Completion = this->PendingFrame;
	const auto RecycleInterfaceFrame = [this, Work]() noexcept
	{
		if (this->UserInterface != nullptr)
			this->UserInterface->RecycleFrame(std::move(Work->InterfaceFrame));
	};
	Completion->Begin();
	bool Scheduled = false;
	try
	{
		Scheduled = Scheduler.TrySchedule(
			[this, Completion, Snapshot, SnapshotScratch, ProjectAssets, ViewportScene, SnapshotOptions, Work,
			 RecycleInterfaceFrame]() mutable
			{
				try
				{
					pipeline::render::SceneRenderSnapshotBuilder::BuildInto(*ViewportScene, *Snapshot, SnapshotOptions, *SnapshotScratch);
					const bool Published = this->ExecutionThread->TryEnqueue(
						[this, Completion, Snapshot, ProjectAssets, Work, RecycleInterfaceFrame]() mutable
						{
							EditorRenderFrame Result = std::move(this->RenderFrameScratch);
							const auto RecycleViewportFrames = [this, &Result]() noexcept
							{
								for (RenderedViewportFrame &ViewportFrame : Result.Viewports)
								{
									EditorViewportInstance *Viewport = this->FindViewport(ViewportFrame.View);
									if (Viewport != nullptr && Viewport->Renderer != nullptr)
										Viewport->Renderer->RecycleFrame(std::move(ViewportFrame.Frame));
								}
								Result.Viewports.clear();
							};
							try
							{
								this->Pipelines->BeginFrame();
								Result.Viewports.clear();
								Result.ReleasedViewports.clear();
								Result.Viewports.reserve(Work->ActiveRequestCount);
								for (usize RequestIndex = 0; RequestIndex < Work->ActiveRequestCount; ++RequestIndex)
								{
									ViewportRenderRequest &Request = Work->Requests[RequestIndex];
									if (Request.Viewport->Renderer == nullptr)
										Request.Viewport->Renderer =
											std::make_unique<viewport::EditorViewportRenderer>(*this->Device, Request.Viewport->View);
									viewport::EditorViewportFrame ViewportFrame = Request.Viewport->Renderer->Render(
										*this->Renderer, *this->Pipelines, *Snapshot, *ProjectAssets, Request.Camera, Request.Extent,
										Request.PickRequests, Work->SelectedObjects, std::move(Request.Gizmo), Request.Settings);
									Result.Viewports.push_back({.View = Request.Viewport->View, .Frame = std::move(ViewportFrame)});
								}
								for (EditorViewportInstance *Viewport : Work->ClosingViewports)
								{
									if (Viewport->Renderer != nullptr)
										Viewport->Renderer->Release(*this->Renderer);
									Viewport->Renderer.reset();
									Result.ReleasedViewports.push_back(Viewport->View);
								}
								this->RenderViewportPresentations.clear();
								this->RenderViewportPresentations.reserve(this->Viewports.size());
								for (const std::unique_ptr<EditorViewportInstance> &Viewport : this->Viewports)
								{
									if (Viewport->Closing)
										continue;
									const auto Rendered = std::ranges::find_if(Result.Viewports, [&](const RenderedViewportFrame &Frame)
																			   { return Frame.View == Viewport->View; });
									this->RenderViewportPresentations.push_back(
										{.View = Viewport->View,
										 .Name = Viewport->Name,
										 .Output = Rendered == Result.Viewports.end() ? Viewport->Output : Rendered->Frame.Output,
										 .Closable = !Viewport->Primary});
								}
								this->Renderer->PrepareInterfacePresentation(*this->Window);
								this->UserInterface->Render(Work->InterfaceFrame, this->RenderViewportPresentations);
								this->Device->InvalidateGraphicsPipelineStateCache();
								this->Window->Present();
								this->UserInterface->RecycleFrame(std::move(Work->InterfaceFrame));
								Completion->Complete(std::move(Result));
							}
							catch (...)
							{
								RecycleViewportFrames();
								RecycleInterfaceFrame();
								this->RenderFrameScratch = std::move(Result);
								Completion->Fail(std::current_exception());
							}
						},
						[Completion, RecycleInterfaceFrame](const std::exception_ptr Failure) mutable
						{
							RecycleInterfaceFrame();
							Completion->Fail(Failure);
						});
					if (!Published)
					{
						RecycleInterfaceFrame();
						Completion->Complete(EditorRenderFrame{});
					}
				}
				catch (...)
				{
					RecycleInterfaceFrame();
					Completion->Fail(std::current_exception());
				}
			},
			core::threading::TaskPriority::Critical);
	}
	catch (...)
	{
		RecycleInterfaceFrame();
		Completion->Fail(std::current_exception());
		throw;
	}
	if (!Scheduled)
	{
		RecycleInterfaceFrame();
		Completion->Complete(EditorRenderFrame{});
	}
}

void EditorLayer::DestroyRenderResources()
{
	if (this->ExecutionThread == nullptr)
		return;
	if (this->ExecutionThread->IsRunning())
	{
		this->ExecutionThread
			->Submit(
				[this]()
				{
					if (this->UserInterface != nullptr)
					{
						this->UserInterface->ShutdownRenderer();
					}
					if (this->Renderer != nullptr)
					{
						for (const std::unique_ptr<EditorViewportInstance> &Viewport : this->Viewports)
						{
							if (Viewport->Renderer != nullptr)
								Viewport->Renderer->Release(*this->Renderer);
							Viewport->Renderer.reset();
						}
					}
					this->Viewports.clear();
					this->Pipelines.reset();
					this->Renderer.reset();
					this->EngineAssets.reset();
				})
			.get();
		this->Session.reset();
		this->ExecutionThread->Stop();
	}
	this->InterfaceRendererInitialized = false;
	this->UserInterface.reset();
	this->ExecutionThread = nullptr;
}

EditorLayer::EditorViewportInstance &EditorLayer::CreateViewport(const pipeline::render::RenderViewID View, const bool Primary)
{
	if (!View.IsValid())
		throw std::invalid_argument("An editor viewport requires a valid render-view identity");
	if (this->FindViewport(View) != nullptr)
		throw std::invalid_argument("An editor viewport identity must be unique");
	auto Instance = std::make_unique<EditorViewportInstance>();
	Instance->View = View;
	Instance->Name = Primary ? "Viewport" : "Viewport " + std::to_string(this->Viewports.size() + 1);
	Instance->Camera = std::make_unique<Camera>(0.1f, 60.0f, 0.05f, 100'000.0f);
	Instance->Camera->Position = glm::vec3(0.0f, 2.0f, 8.0f);
	Instance->Camera->Yaw = -90.0f;
	Instance->Camera->Pitch = -10.0f;
	Instance->Camera->UpdateCameraVectors();
	Instance->Primary = Primary;
	EditorViewportInstance &Result = *Instance;
	this->Viewports.push_back(std::move(Instance));
	return Result;
}

EditorLayer::EditorViewportInstance *EditorLayer::FindViewport(const pipeline::render::RenderViewID View) noexcept
{
	const auto Result = std::ranges::find_if(this->Viewports, [View](const auto &Viewport) { return Viewport->View == View; });
	return Result == this->Viewports.end() ? nullptr : Result->get();
}

std::span<const ui::EditorViewportPresentation> EditorLayer::BuildViewportPresentations() const
{
	std::vector<ui::EditorViewportPresentation> &Result = this->OwnerViewportPresentations;
	Result.clear();
	Result.reserve(this->Viewports.size());
	for (const std::unique_ptr<EditorViewportInstance> &Viewport : this->Viewports)
	{
		if (Viewport->Closing)
			continue;
		Result.push_back({.View = Viewport->View,
						  .Name = Viewport->Name,
						  .Output = Viewport->Output,
						  .Projection = Viewport->Camera->Projection,
						  .Closable = !Viewport->Primary});
	}
	return Result;
}
} // namespace editor
