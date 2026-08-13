#include "GameLayer.h"

#include "Source/component/object/CObjectCameraComponent.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/core/app/ApplicationServices.h"
#include "Source/core/io/SecurePath.h"
#include "Source/core/window/Window.h"
#include "Source/pipeline/device/Device.h"
#include "Source/runtime/project/RuntimeSceneLoader.h"

#include <thread>
#include <utility>

namespace core
{
namespace
{
[[nodiscard]] std::filesystem::path ResolveWithin(const std::filesystem::path &Root, const std::filesystem::path &Relative,
												  const string_view Field)
{
	if (Relative.empty() || Relative.is_absolute())
		throw GameLayerException(string(Field) + " must be a non-empty relative path");
	try
	{
		return core::io::SecurePath::ResolveWithin(Root, Relative, Field);
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw GameLayerException(string(Field) + " is not securely contained: " + Exception.what());
	}
}
} // namespace

GameLayer::GameLayer(core::Window *Window, pipeline::device::Device &Device, GameLayerSpecification Specification)
	: Window(Window), Device(&Device), HeadlessPresentationValidation(Specification.HeadlessPresentationValidation),
	  HeadlessPresentationCapturePath(std::move(Specification.HeadlessPresentationCapturePath))
{
	if (Window == nullptr)
		throw std::invalid_argument("GameLayer requires an application window");
	if (Specification.ProjectName.empty() || Specification.ContentRoot.empty() || Specification.EngineContentRoot.empty() ||
		Specification.StartupScene.empty() || Specification.CacheRoot.empty())
	{
		throw std::invalid_argument("GameLayer requires explicit project, content, engine, startup-scene, and cache settings");
	}
	if (!std::filesystem::is_directory(Specification.ContentRoot))
		throw GameLayerException("Project content root does not exist");
	if (!std::filesystem::is_directory(Specification.EngineContentRoot / "Shaders"))
		throw GameLayerException("Engine content root does not contain the shader directory");

	this->ProjectAssets = std::make_unique<resource::AssetManager>(std::move(Specification.ContentRoot));
	this->EngineAssets = std::make_unique<resource::AssetManager>(std::move(Specification.EngineContentRoot));
	this->BehaviorRegistry = std::make_unique<runtime::behavior::BehaviorRegistry>();
	this->GameModuleManager = std::make_unique<runtime::module::GameModuleManager>(*this->BehaviorRegistry);
	if (!Specification.GameModule.empty())
	{
		this->GameModuleManager->Configure(std::move(Specification.GameModule), Specification.CacheRoot);
		if (!this->GameModuleManager->ForceReload(true))
			throw GameLayerException("Could not load the packaged game module: " + this->GameModuleManager->GetDiagnostic());
	}

	const std::filesystem::path ScenePath = ResolveWithin(this->ProjectAssets->GetRootPath(), Specification.StartupScene, "StartupScene");
	runtime::project::LoadedRuntimeScene LoadedScene = runtime::project::RuntimeSceneLoader::Load(ScenePath, *this->ProjectAssets);
	this->Session =
		std::make_unique<runtime::simulation::GameSession>(std::move(LoadedScene.Scene), *this->ProjectAssets, *this->BehaviorRegistry);
	this->Session->Start();

	this->PrimaryCamera = std::make_unique<Camera>(0.0f, 60.0f, 0.05f, 100'000.0f);
	this->SynchronizePrimaryCamera();
	for (std::unique_ptr<RenderFrameSlot> &Slot : this->RenderFrameSlots)
		Slot = std::make_unique<RenderFrameSlot>(*this->PrimaryCamera);
	this->Window->SetCursorMode(core::CursorMode::Relative);
}

GameLayer::~GameLayer()
{
	try
	{
		this->DestroyResources();
	}
	catch (...)
	{
		std::terminate();
	}
}

void GameLayer::Run(const core::ApplicationFrame &Frame)
{
	this->RethrowSnapshotFailure();
	this->StartRenderThread(Frame.Services);
	this->Session->Tick(Frame.Services.GetTaskScheduler(), Frame.Timing.DeltaSeconds);
	world::Scene &Scene = this->Session->GetScene();
	this->AnimationSystem.Update(Scene, static_cast<float32>(Frame.Timing.DeltaSeconds));
	this->SynchronizePrimaryCamera();
	if (!Frame.FramebufferExtent.IsValid())
		return;
	RenderFrameSlot *AvailableSlot = nullptr;
	for (const std::unique_ptr<RenderFrameSlot> &Slot : this->RenderFrameSlots)
	{
		bool Expected = false;
		if (Slot->InUse.compare_exchange_strong(Expected, true, std::memory_order_acq_rel))
		{
			AvailableSlot = Slot.get();
			break;
		}
	}
	if (AvailableSlot == nullptr)
		return;
	AvailableSlot->Frame.View = *this->PrimaryCamera;
	AvailableSlot->Frame.Extent = Frame.FramebufferExtent;
	this->PendingSnapshotTasks.fetch_add(1, std::memory_order_acq_rel);
	const bool Scheduled = Frame.Services.GetTaskScheduler().TrySchedule(
		[this, AvailableSlot, Scene = &Scene]()
		{
			try
			{
				pipeline::render::SceneRenderSnapshotBuilder::BuildInto(*Scene, AvailableSlot->Frame.Scene, {},
																		AvailableSlot->Frame.SceneScratch);
				if (!this->ExecutionThread->TryEnqueue(
						[this, AvailableSlot]()
						{
							struct Completion final
							{
								RenderFrameSlot &Slot;
								~Completion()
								{
									this->Slot.InUse.store(false, std::memory_order_release);
								}
							} CompletionScope{*AvailableSlot};
							try
							{
								this->RunOnRenderThread(AvailableSlot->Frame);
								this->Window->Present();
							}
							catch (...)
							{
								this->RecordSnapshotFailure(std::current_exception());
							}
						},
						[this, AvailableSlot](const std::exception_ptr Failure)
						{
							AvailableSlot->InUse.store(false, std::memory_order_release);
							this->RecordSnapshotFailure(Failure);
						}))
				{
					AvailableSlot->InUse.store(false, std::memory_order_release);
				}
			}
			catch (...)
			{
				AvailableSlot->InUse.store(false, std::memory_order_release);
				this->RecordSnapshotFailure(std::current_exception());
			}
			const uint32 Previous = this->PendingSnapshotTasks.fetch_sub(1, std::memory_order_acq_rel);
			if (Previous == 1)
				this->PendingSnapshotTasks.notify_all();
		},
		core::threading::TaskPriority::Critical);
	if (!Scheduled)
	{
		AvailableSlot->InUse.store(false, std::memory_order_release);
		const uint32 Previous = this->PendingSnapshotTasks.fetch_sub(1, std::memory_order_acq_rel);
		if (Previous == 1)
			this->PendingSnapshotTasks.notify_all();
	}
	if (this->HeadlessPresentationValidation)
	{
		while (AvailableSlot->InUse.load(std::memory_order_acquire))
			std::this_thread::yield();
		this->RethrowSnapshotFailure();
	}
}

void GameLayer::StartRenderThread(core::ApplicationServices &Services)
{
	if (this->ExecutionThread != nullptr)
		return;
	pipeline::render::RenderPipelineLibrary::PreloadShaderSources(*this->EngineAssets, Services.GetTaskScheduler());
	this->ExecutionThread = &Services.GetRenderThread();
	this->ExecutionThread->Start(this->Window->GetContext());
	this->ExecutionThread
		->Submit(
			[this]()
			{
				this->Renderer = std::make_unique<pipeline::render::Renderer>(*this->Device, this->HeadlessPresentationValidation,
																			  this->HeadlessPresentationCapturePath);
				this->Pipelines = std::make_unique<pipeline::render::RenderPipelineLibrary>(*this->Device, *this->EngineAssets,
																							!this->Window->IsSRGBPresentationCapable());
			})
		.get();
}

void GameLayer::RunOnRenderThread(const RenderFrame &Frame)
{
	this->Pipelines->BeginFrame();
	this->Renderer->Render(Frame.Scene, *this->ProjectAssets, Frame.View, this->View);
	const pipeline::render::RenderViewOutput Output = this->Renderer->RenderView(
		this->Pipelines->GetPipelineSet(), Frame.View, pipeline::render::RenderViewDescriptor{.View = this->View, .Extent = Frame.Extent});
	if (Output.IsValid())
		this->Renderer->PresentView(Output, *this->Window);
	else if (this->HeadlessPresentationValidation)
		throw GameLayerException("Headless game-layer rendering produced an invalid view output");
}

void GameLayer::SynchronizePrimaryCamera()
{
	world::Scene &Scene = this->Session->GetScene();
	const auto Access = Scene.Read();
	const components::CObjectCameraComponent *SelectedCamera = nullptr;
	const components::CObjectTransformComponent *SelectedTransform = nullptr;
	for (const components::CObjectCameraComponent &Candidate : Access.Components<components::CObjectCameraComponent>())
	{
		if (!Candidate.IsEnabled() || !Candidate.IsPrimary())
			continue;
		if (SelectedCamera != nullptr)
			throw GameLayerException("Runtime scene contains more than one enabled primary camera");
		const auto TransformHandle = Access.GetComponent<components::CObjectTransformComponent>(Candidate.GetOwner());
		if (!TransformHandle.IsValid())
			throw GameLayerException("Primary camera does not have its required transform component");
		SelectedCamera = &Candidate;
		SelectedTransform = &Access.Resolve(TransformHandle);
	}
	if (SelectedCamera == nullptr || SelectedTransform == nullptr)
		throw GameLayerException("Runtime scene requires exactly one enabled primary camera");

	this->PrimaryCamera->Position = SelectedTransform->GetPosition();
	this->PrimaryCamera->Front = SelectedTransform->GetForward();
	this->PrimaryCamera->Up = SelectedTransform->GetUp();
	this->PrimaryCamera->Right = SelectedTransform->GetRight();
	this->PrimaryCamera->WorldUp = this->PrimaryCamera->Up;
	this->PrimaryCamera->FOV = SelectedCamera->GetVerticalFieldOfViewDegrees();
	this->PrimaryCamera->NearPlane = SelectedCamera->GetNearPlane();
	this->PrimaryCamera->FarPlane = SelectedCamera->GetFarPlane();
	this->PrimaryCamera->Projection = SelectedCamera->GetProjection() == components::CameraProjection::Orthographic
										  ? CameraProjectionMode::Orthographic
										  : CameraProjectionMode::Perspective;
	this->PrimaryCamera->OrthographicHeight = SelectedCamera->GetOrthographicHeight();
	this->PrimaryCamera->ExposureCompensation = SelectedCamera->GetExposureCompensation();
	this->PrimaryCamera->TemporalJitterEnabled = SelectedCamera->IsTemporalJitterEnabled();
}

void GameLayer::DestroyResources()
{
	this->WaitForSnapshotTasks();
	if (this->Session != nullptr)
		this->Session->Stop();
	if (this->ExecutionThread != nullptr && this->ExecutionThread->IsRunning())
	{
		this->ExecutionThread
			->Submit(
				[this]()
				{
					this->Pipelines.reset();
					this->Renderer.reset();
					this->PrimaryCamera.reset();
					this->Session.reset();
					this->EngineAssets.reset();
					this->ProjectAssets.reset();
				})
			.get();
		this->ExecutionThread->Stop();
	}
	else
	{
		this->Pipelines.reset();
		this->Renderer.reset();
		this->PrimaryCamera.reset();
		this->Session.reset();
		this->EngineAssets.reset();
		this->ProjectAssets.reset();
	}
	this->GameModuleManager.reset();
	this->BehaviorRegistry.reset();
	this->ExecutionThread = nullptr;
}

void GameLayer::RecordSnapshotFailure(std::exception_ptr Failure) noexcept
{
	if (Failure == nullptr)
		return;
	std::scoped_lock Lock(this->SnapshotFailureMutex);
	if (this->SnapshotFailure == nullptr)
		this->SnapshotFailure = std::move(Failure);
}

void GameLayer::RethrowSnapshotFailure()
{
	std::exception_ptr Failure;
	{
		std::scoped_lock Lock(this->SnapshotFailureMutex);
		Failure = std::exchange(this->SnapshotFailure, nullptr);
	}
	if (Failure != nullptr)
		std::rethrow_exception(Failure);
}

void GameLayer::WaitForSnapshotTasks() noexcept
{
	uint32 Pending = this->PendingSnapshotTasks.load(std::memory_order_acquire);
	while (Pending != 0)
	{
		this->PendingSnapshotTasks.wait(Pending, std::memory_order_acquire);
		Pending = this->PendingSnapshotTasks.load(std::memory_order_acquire);
	}
}
} // namespace core
