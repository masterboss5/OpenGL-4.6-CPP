#include "src/core/app/Application.h"
#include "src/core/layers/EditorLayer.h"
#include "src/core/layers/GameLayer.h"
#include "src/core/threading/RenderThread.h"
#include "src/core/window/Context.h"
#include "src/core/window/WindowManager.h"
#include "src/component/object/CObjectCameraComponent.h"
#include "src/component/object/CObjectLightComponents.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/editor/document/SceneDocument.h"
#include "src/editor/reflection/ComponentReflection.h"
#include "src/editor/reflection/ReflectionRegistry.h"
#include "src/editor/serialization/ProjectDescriptorSerializer.h"
#include "src/editor/serialization/SceneDocumentSerializer.h"
#include "src/editor/validation/EditorCoreValidation.h"
#include "src/pipeline/validation/RenderCoreValidation.h"
#include "src/resource/asset/AssetManager.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#ifdef CreateWindow
#undef CreateWindow
#endif

namespace
{
class StopValidationLayer final : public ApplicationLayer
{
  public:
	StopValidationLayer(core::Window *, pipeline::device::Device &, std::atomic<uint32> &RunCount) : RunCount(RunCount)
	{
	}

	void Run(const core::ApplicationFrame &Frame) override
	{
		this->RunCount.fetch_add(1, std::memory_order_relaxed);
		Frame.Services.RequestStop();
	}

  private:
	std::atomic<uint32> &RunCount;
};

class StopSentinelLayer final : public ApplicationLayer
{
  public:
	StopSentinelLayer(core::Window *, pipeline::device::Device &, std::atomic<uint32> &RunCount) : RunCount(RunCount)
	{
	}

	void Run(const core::ApplicationFrame &) override
	{
		this->RunCount.fetch_add(1, std::memory_order_relaxed);
	}

  private:
	std::atomic<uint32> &RunCount;
};

void ValidateDetachedWindowLifecycle(core::WindowManager &Manager)
{
	core::Window &PrimaryWindow = Manager.GetPrimaryWindow();
	const core::MonitorInfo PrimaryMonitor = Manager.GetPrimaryMonitor();
	if (!std::isfinite(PrimaryMonitor.ContentScaleX) || !std::isfinite(PrimaryMonitor.ContentScaleY) ||
		PrimaryMonitor.ContentScaleX <= 0.0f || PrimaryMonitor.ContentScaleY <= 0.0f)
	{
		throw std::runtime_error("Detached-window validation received invalid monitor DPI scaling");
	}

	core::WindowSpecification Specification;
	Specification.Title = "Detached Window Lifecycle Validation";
	Specification.Extent = {640, 480};
	Specification.ContextGroup = PrimaryWindow.GetContext().GetShareGroupName();
	Specification.Visible = false;
	Specification.Focused = false;
	Specification.HeadlessValidation = true;
	core::Window &DetachedWindow = Manager.CreateWindow(Specification);
	const core::WindowID DetachedWindowID = DetachedWindow.GetID();
	if (DetachedWindowID == PrimaryWindow.GetID() || Manager.FindManagedWindow(DetachedWindowID) != &DetachedWindow ||
		!std::isfinite(DetachedWindow.GetContentScale()) || DetachedWindow.GetContentScale() <= 0.0f)
	{
		throw std::runtime_error("Detached window was not registered with a valid independent identity and DPI scale");
	}
	std::atomic<bool> ForeignLookupRejected = false;
	std::thread ForeignLookup(
		[&Manager, DetachedWindowID, &ForeignLookupRejected]()
		{
			try
			{
				(void)Manager.FindManagedWindow(DetachedWindowID);
			}
			catch (const core::WindowException &)
			{
				ForeignLookupRejected.store(true, std::memory_order_release);
			}
		});
	ForeignLookup.join();
	if (!ForeignLookupRejected.load(std::memory_order_acquire))
		throw std::runtime_error("WindowManager allowed a borrowed Window pointer to escape onto a foreign thread");

	core::Context &DetachedContext = DetachedWindow.GetContext();
	core::threading::RenderThread RenderThread({.QueueCapacity = 16});
	RenderThread.Start(DetachedContext);
	const bool Adopted = RenderThread
							 .Submit([&DetachedContext, DetachedWindowID]()
									 { return DetachedContext.IsCurrent() && DetachedContext.GetWindowID() == DetachedWindowID; })
							 .get();
	RenderThread.Stop();
	if (!Adopted || !DetachedContext.IsCurrent() || DetachedContext.IsThreadTransferPending())
		throw std::runtime_error("Detached window context did not complete its render-thread transfer and owner-thread return");

	DetachedContext.MarkReset();
	bool ResetRejected = false;
	try
	{
		DetachedContext.RequireCurrentThread();
	}
	catch (const core::ContextException &)
	{
		ResetRejected = true;
	}
	if (!ResetRejected)
		throw std::runtime_error("Reset detached context remained usable after a simulated device failure");

	Manager.DestroyWindow(DetachedWindowID);
	if (Manager.FindManagedWindow(DetachedWindowID) != nullptr)
		throw std::runtime_error("Detached window survived its owner-thread shutdown");
}

class TemporaryEditorProject final
{
  public:
	TemporaryEditorProject()
	{
		this->Root = std::filesystem::temp_directory_path() / ("EditorLayerValidation-" + util::UUID::GenerateRandomUUID().ToString());
		std::filesystem::create_directories(this->Root / "Content");
		this->DescriptorPath = this->Root / "Validation.engineproject";
		std::ofstream Descriptor(this->DescriptorPath, std::ios::binary | std::ios::trunc);
		if (!Descriptor)
			throw std::runtime_error("Could not create the editor-layer validation descriptor");
		Descriptor << "{}";
		if (!Descriptor)
			throw std::runtime_error("Could not write the editor-layer validation descriptor");
	}

	~TemporaryEditorProject()
	{
		std::error_code Error;
		std::filesystem::remove_all(this->Root, Error);
	}

	[[nodiscard]] const std::filesystem::path &GetDescriptorPath() const noexcept
	{
		return this->DescriptorPath;
	}

  private:
	std::filesystem::path Root;
	std::filesystem::path DescriptorPath;
};

class TemporaryGameProject final
{
  public:
	TemporaryGameProject()
	{
		this->Root = std::filesystem::temp_directory_path() / ("GameLayerValidation-" + util::UUID::GenerateRandomUUID().ToString());
		const std::filesystem::path ContentRoot = this->Root / "Content";
		const std::filesystem::path ScenePath = ContentRoot / "Scenes" / "Startup.enginelevel";
		const std::filesystem::path ModelPath = ContentRoot / "Models" / "ValidationCube.obj";
		this->DescriptorPath = this->Root / "Validation.engineproject";
		std::filesystem::create_directories(ScenePath.parent_path());
		std::filesystem::create_directories(ModelPath.parent_path());
		std::ofstream Model(ModelPath, std::ios::binary | std::ios::trunc);
		if (!Model)
			throw std::runtime_error("Could not create the game-layer validation model");
		Model << "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
				 "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
				 "f 1 4 3 2\nf 5 6 7 8\nf 1 5 8 4\nf 2 3 7 6\nf 1 2 6 5\nf 4 8 7 3\n";
		Model.flush();
		if (!Model)
			throw std::runtime_error("Could not write the game-layer validation model");
		Model.close();

		resource::AssetManager Assets(ContentRoot);
		editor::reflection::ReflectionRegistry Reflection;
		editor::reflection::RegisterCoreComponentReflection(Reflection);
		editor::document::SceneDocument Document("Game Layer Validation");
		const world::ObjectHandle CameraObject = Document.CreateObject("Primary Camera");
		const world::ComponentHandle<components::CObjectCameraComponent> Camera =
			Document.GetScene().AddComponent<components::CObjectCameraComponent>(CameraObject);
		const world::ComponentHandle<components::CObjectTransformComponent> CameraTransform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(CameraObject);
		const world::ObjectHandle ModelObject = Document.CreateObject("Validation Cube");
		(void)Document.GetScene().AddComponent<components::CObjectMeshComponent>(
			ModelObject, Assets.GetAsset<resource::ModelAsset>("Models/ValidationCube.obj"));
		const world::ObjectHandle LightObject = Document.CreateObject("Validation Light");
		const world::ComponentHandle<components::CObjectPointLightComponent> Light =
			Document.GetScene().AddComponent<components::CObjectPointLightComponent>(LightObject);
		const world::ComponentHandle<components::CObjectTransformComponent> LightTransform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(LightObject);
		{
			auto Access = Document.GetScene().Write();
			Access.Resolve(Camera).SetPrimary(true);
			Access.Resolve(CameraTransform).SetPosition({0.0f, 2.0f, 8.0f});
			Access.Resolve(CameraTransform).LookAt({0.0f, 0.0f, 0.0f});
			Access.Resolve(Light).SetLuminousPowerLumens(12'000.0f);
			Access.Resolve(Light).SetRange(30.0f);
			Access.Resolve(Light).GetShadowSettings().CastShadows = false;
			Access.Resolve(LightTransform).SetPosition({2.0f, 3.0f, 4.0f});
		}
		editor::serialization::SceneDocumentSerializer::Save(Document, Reflection, Assets, ScenePath);
		editor::serialization::ProjectDescriptorSerializer::Save(
			{.Name = "Game Layer Validation", .DescriptorPath = this->DescriptorPath, .StartupScene = "Scenes/Startup.enginelevel"});
	}

	~TemporaryGameProject()
	{
		std::error_code Error;
		std::filesystem::remove_all(this->Root, Error);
	}

	[[nodiscard]] const std::filesystem::path &GetDescriptorPath() const noexcept
	{
		return this->DescriptorPath;
	}

	[[nodiscard]] const std::filesystem::path &GetRoot() const noexcept
	{
		return this->Root;
	}

	[[nodiscard]] static std::filesystem::path GetPresentationCapturePath()
	{
#ifdef _DEBUG
		constexpr string_view Configuration = "Debug";
#else
		constexpr string_view Configuration = "Release";
#endif
		return std::filesystem::current_path().parent_path() / "x64" / "ValidationEvidence" /
			   (string(Configuration) + "-HeadlessPresentation.bmp");
	}

  private:
	std::filesystem::path Root;
	std::filesystem::path DescriptorPath;
};

[[nodiscard]] core::WindowSpecification MakeValidationWindow(const string &Title)
{
	core::WindowSpecification Window;
	Window.Title = Title;
	Window.Extent = {640, 360};
	Window.Visible = false;
	Window.Focused = false;
	Window.HeadlessValidation = true;
	return Window;
}

[[nodiscard]] std::filesystem::path FindEngineContentRoot()
{
	const std::array Candidates{std::filesystem::current_path(), std::filesystem::current_path() / "OpenGL"};
	for (const std::filesystem::path &Candidate : Candidates)
	{
		std::error_code Error;
		if (std::filesystem::is_directory(Candidate / "shader", Error) && !Error)
			return std::filesystem::absolute(Candidate).lexically_normal();
	}
	throw std::runtime_error("Validation could not locate the engine shader content root");
}
} // namespace

int wmain(const int ArgumentCount, wchar_t *Arguments[])
{
	try
	{
		bool RunEditorCore = ArgumentCount == 1;
		bool RunRenderCore = ArgumentCount == 1;
		bool RunEditorLayer = false;
		bool RunGameLayer = false;
		std::filesystem::path ValidGameModule;
		std::filesystem::path InvalidGameModule;
		std::filesystem::path MSBuild;
		std::filesystem::path BuildSolution;
		std::filesystem::path BuiltGameModule;
		string BuildConfiguration;

		for (int32 ArgumentIndex = 1; ArgumentIndex < ArgumentCount; ++ArgumentIndex)
		{
			const std::wstring_view Argument = Arguments[ArgumentIndex];
			if (Argument == L"--all")
			{
				RunEditorCore = true;
				RunRenderCore = true;
				RunEditorLayer = true;
				RunGameLayer = true;
			}
			else if (Argument == L"--editor-core")
				RunEditorCore = true;
			else if (Argument == L"--render-core")
				RunRenderCore = true;
			else if (Argument == L"--editor-layer")
				RunEditorLayer = true;
			else if (Argument == L"--game-layer")
				RunGameLayer = true;
			else if (Argument == L"--game-module")
			{
				if (ArgumentIndex + 2 >= ArgumentCount)
					throw std::invalid_argument("--game-module requires valid and invalid DLL paths");
				ValidGameModule = Arguments[++ArgumentIndex];
				InvalidGameModule = Arguments[++ArgumentIndex];
			}
			else if (Argument == L"--project-build")
			{
				if (ArgumentIndex + 4 >= ArgumentCount)
					throw std::invalid_argument("--project-build requires MSBuild, solution, built DLL, and configuration arguments");
				MSBuild = Arguments[++ArgumentIndex];
				BuildSolution = Arguments[++ArgumentIndex];
				BuiltGameModule = Arguments[++ArgumentIndex];
				const std::wstring_view WideConfiguration = Arguments[++ArgumentIndex];
				if (!std::ranges::all_of(WideConfiguration, [](const wchar_t Character) { return Character >= 0 && Character <= 0x7f; }))
					throw std::invalid_argument("--project-build configuration must be ASCII");
				BuildConfiguration.resize(WideConfiguration.size());
				std::ranges::transform(WideConfiguration, BuildConfiguration.begin(),
									   [](const wchar_t Character) { return static_cast<char>(Character); });
			}
			else
				throw std::invalid_argument("Unknown Validation argument");
		}

		if (RunEditorCore)
			editor::validation::RunDeterministicEditorCoreChecks();
		if (!ValidGameModule.empty())
			editor::validation::RunDeterministicGameModuleChecks(ValidGameModule, InvalidGameModule);
		if (!MSBuild.empty())
			editor::validation::RunDeterministicProjectBuildChecks(MSBuild, BuildSolution, BuiltGameModule, std::move(BuildConfiguration));
		if (RunRenderCore)
		{
			core::Application Application({.Window = MakeValidationWindow("Render Core Validation"), .MaximumFrameCount = 1});
			pipeline::device::Device &Device = Application.GetWindowManager().GetDevice(Application.GetPrimaryWindow());
			pipeline::validation::RunDeterministicRenderCoreChecks(Device);
			std::cerr << "[Validation] Detached window, DPI, and reset-context shutdown\n";
			ValidateDetachedWindowLifecycle(Application.GetWindowManager());
			std::atomic<uint32> StopLayerRuns = 0;
			std::atomic<uint32> StopSentinelRuns = 0;
			Application.PushLayer<StopValidationLayer>(StopLayerRuns);
			Application.PushLayer<StopSentinelLayer>(StopSentinelRuns);
			Application.Main();
			if (StopLayerRuns.load(std::memory_order_acquire) != 1 || StopSentinelRuns.load(std::memory_order_acquire) != 0)
				throw std::runtime_error("application stop request did not terminate layer dispatch immediately");
		}
		if (RunEditorLayer)
		{
			std::cerr << "[Validation] Editor layer project setup\n";
			TemporaryEditorProject Project;
			std::cerr << "[Validation] Editor layer application setup\n";
			core::Application Application({.Window = MakeValidationWindow("Editor Layer Validation")});
			std::cerr << "[Validation] Editor layer construction\n";
			editor::EditorLayer &Layer = Application.PushLayer<editor::EditorLayer>(
				editor::EditorLayerSpecification{.Project = {.Name = "Validation", .DescriptorPath = Project.GetDescriptorPath()},
												 .EngineContentRoot = FindEngineContentRoot(),
												 .MaximumRenderedFrames = 2});
			(void)Layer.GetSession().GetDocument().CreateObject("Gizmo validation object");
			(void)Layer.GetSession().QueueViewportPick({.Value = 2}, 0.5f, 0.5f, editor::viewport::SelectionOperation::Replace);
			std::cerr << "[Validation] Editor layer frames\n";
			Application.Main();
			std::cerr << "[Validation] Editor layer shutdown\n";
		}
		if (RunGameLayer)
		{
			TemporaryGameProject Project;
			const std::filesystem::path CapturePath = TemporaryGameProject::GetPresentationCapturePath();
			std::error_code RemoveError;
			std::filesystem::remove(CapturePath, RemoveError);
			{
				core::Application Application({.Window = MakeValidationWindow("Game Layer Validation"), .MaximumFrameCount = 8});
				Application.PushLayer<core::GameLayer>(core::GameLayerSpecification{.ProjectName = "Game Layer Validation",
																					.ContentRoot = Project.GetRoot() / "Content",
																					.EngineContentRoot = FindEngineContentRoot(),
																					.StartupScene = "Scenes/Startup.enginelevel",
																					.CacheRoot = Project.GetRoot() / "Intermediate",
																					.HeadlessPresentationValidation = true,
																					.HeadlessPresentationCapturePath = CapturePath});
				Application.Main();
			}
			std::error_code CaptureError;
			if (!std::filesystem::is_regular_file(CapturePath, CaptureError) || CaptureError ||
				std::filesystem::file_size(CapturePath) <= 32U)
				throw std::runtime_error("Game-layer rendering smoke test did not produce a validated presentation capture");
		}

		std::cout << "Validation completed successfully\n";
		return EXIT_SUCCESS;
	}
	catch (const std::exception &Exception)
	{
		std::cerr << "Validation failed: " << Exception.what() << '\n';
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::cerr << "Validation failed: unknown non-standard exception\n";
		return EXIT_FAILURE;
	}
}
