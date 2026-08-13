#pragma once

#include "Source/concepts.h"
#include "Source/core/app/ApplicationServices.h"
#include "Source/core/diagnostics/DiagnosticSink.h"
#include "Source/core/input/InputSystem.h"
#include "Source/core/layers/ApplicationLayer.h"
#include "Source/core/threading/RenderThread.h"
#include "Source/core/threading/TaskScheduler.h"
#include "Source/core/time/FrameClock.h"
#include "Source/core/window/WindowManager.h"
#include "Source/types.h"

#include <memory>
#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace core
{
struct ApplicationSpecification final
{
	WindowSpecification Window;
	uint64 MaximumFrameCount = 0;
};

class ENGINE_API Application final
{
  private:
	std::atomic<bool> Running = false;
	core::WindowManager WindowManager;
	input::InputSystem InputSystem;
	FrameClock FrameClock;
	threading::TaskScheduler TaskScheduler;
	threading::RenderThread RenderThread;
	diagnostics::DiagnosticSink Diagnostics;
	ApplicationServices Services;
	core::Window *Window = nullptr;
	uint64 MaximumFrameCount = 0;
	std::vector<WindowEvent> WindowEventsScratch;
	std::vector<std::unique_ptr<ApplicationLayer>> Layers = {};
	std::vector<std::unique_ptr<ApplicationLayer>> PendingLayers = {};
	std::thread::id ApplicationThreadID;
	bool DispatchingLayers = false;

	void QueueLayer(std::unique_ptr<ApplicationLayer> Layer);
	void CommitPendingLayers();

  public:
	explicit Application(ApplicationSpecification Specification = {});
	explicit Application(WindowSpecification WindowSpecification);
	~Application();

	void Main();
	void Stop();
	void RequireApplicationThread() const;
	[[nodiscard]] bool IsApplicationThread() const noexcept;
	[[nodiscard]] usize GetLayerCount() const;
	[[nodiscard]] core::Window &GetPrimaryWindow() const;
	[[nodiscard]] core::WindowManager &GetWindowManager();
	[[nodiscard]] input::InputSystem &GetInputSystem();

	template <IsApplicationLayer TLayer, typename... ArgumentTypes>
		requires std::constructible_from<TLayer, core::Window *, pipeline::device::Device &, ArgumentTypes...>
	TLayer &PushLayer(ArgumentTypes &&...Arguments)
	{
		this->RequireApplicationThread();
		auto Layer =
			std::make_unique<TLayer>(this->Window, this->WindowManager.GetDevice(*this->Window), std::forward<ArgumentTypes>(Arguments)...);
		TLayer &Reference = *Layer;
		this->QueueLayer(std::move(Layer));
		return Reference;
	}
};
} // namespace core
