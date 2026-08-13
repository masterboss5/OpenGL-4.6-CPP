#pragma once

#include "Source/animation/AnimationSystem.h"
#include "Source/core/layers/ApplicationLayer.h"
#include "Source/core/threading/RenderThread.h"
#include "Source/pipeline/render/RenderPipelineLibrary.h"
#include "Source/pipeline/render/Renderer.h"
#include "Source/runtime/behavior/BehaviorRegistry.h"
#include "Source/runtime/module/GameModule.h"
#include "Source/runtime/simulation/GameSession.h"
#include "Source/scene/Camera.h"

#include <filesystem>
#include <array>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace core
{
class ApplicationServices;
class Window;
} // namespace core

namespace pipeline::device
{
class Device;
}

namespace core
{
struct GameLayerSpecification final
{
	string ProjectName;
	std::filesystem::path ContentRoot;
	std::filesystem::path EngineContentRoot;
	std::filesystem::path StartupScene;
	std::filesystem::path GameModule;
	std::filesystem::path CacheRoot;
	bool HeadlessPresentationValidation = false;
	std::filesystem::path HeadlessPresentationCapturePath;
};

class GameLayerException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class GameLayer final : public ApplicationLayer
{
  public:
	GameLayer(core::Window *Window, pipeline::device::Device &Device, GameLayerSpecification Specification);
	~GameLayer() override;

	GameLayer(const GameLayer &) = delete;
	GameLayer &operator=(const GameLayer &) = delete;
	GameLayer(GameLayer &&) = delete;
	GameLayer &operator=(GameLayer &&) = delete;

	void Run(const core::ApplicationFrame &Frame) override;

  private:
	struct RenderFrame final
	{
		pipeline::render::SceneRenderSnapshot Scene;
		pipeline::render::SceneRenderSnapshotBuildScratch SceneScratch;
		Camera View;
		core::WindowExtent Extent;
	};
	struct RenderFrameSlot final
	{
		explicit RenderFrameSlot(const Camera &View) : Frame{.View = View}
		{
		}

		RenderFrame Frame;
		std::atomic<bool> InUse = false;
	};

	void StartRenderThread(core::ApplicationServices &Services);
	void RunOnRenderThread(const RenderFrame &Frame);
	void SynchronizePrimaryCamera();
	void DestroyResources();
	void RecordSnapshotFailure(std::exception_ptr Failure) noexcept;
	void RethrowSnapshotFailure();
	void WaitForSnapshotTasks() noexcept;

	core::Window *Window = nullptr;
	pipeline::device::Device *Device = nullptr;
	std::unique_ptr<resource::AssetManager> ProjectAssets;
	std::unique_ptr<resource::AssetManager> EngineAssets;
	std::unique_ptr<runtime::behavior::BehaviorRegistry> BehaviorRegistry;
	std::unique_ptr<runtime::module::GameModuleManager> GameModuleManager;
	std::unique_ptr<runtime::simulation::GameSession> Session;
	std::unique_ptr<Camera> PrimaryCamera;
	std::unique_ptr<pipeline::render::Renderer> Renderer;
	std::unique_ptr<pipeline::render::RenderPipelineLibrary> Pipelines;
	pipeline::render::RenderViewID View{.Value = 1};
	animation::AnimationSystem AnimationSystem;
	core::threading::RenderThread *ExecutionThread = nullptr;
	std::array<std::unique_ptr<RenderFrameSlot>, 3> RenderFrameSlots;
	std::atomic<uint32> PendingSnapshotTasks = 0;
	std::mutex SnapshotFailureMutex;
	std::exception_ptr SnapshotFailure;
	bool HeadlessPresentationValidation = false;
	std::filesystem::path HeadlessPresentationCapturePath;
};
} // namespace core
