#pragma once

#include "src/core/EngineAPI.h"

namespace core
{
class Application;
class Window;
class WindowManager;

namespace input
{
class InputSystem;
}

namespace diagnostics
{
class DiagnosticSink;
}

namespace threading
{
class RenderThread;
class TaskScheduler;
} // namespace threading

class ENGINE_API ApplicationServices final
{
  public:
	[[nodiscard]] Window &GetPrimaryWindow() const;
	[[nodiscard]] WindowManager &GetWindowManager() const;
	[[nodiscard]] input::InputSystem &GetInputSystem() const;
	[[nodiscard]] threading::TaskScheduler &GetTaskScheduler() const noexcept;
	[[nodiscard]] threading::RenderThread &GetRenderThread() const noexcept;
	[[nodiscard]] diagnostics::DiagnosticSink &GetDiagnostics() const noexcept;
	void RequestStop() const noexcept;
	void RequireApplicationThread() const;
	[[nodiscard]] bool IsApplicationThread() const noexcept;

  private:
	friend class Application;

	ApplicationServices(Application &Application, WindowManager &WindowManager, input::InputSystem &InputSystem,
						threading::TaskScheduler &TaskScheduler, threading::RenderThread &RenderThread,
						diagnostics::DiagnosticSink &Diagnostics) noexcept;

	Application *ApplicationInstance = nullptr;
	WindowManager *WindowManagerInstance = nullptr;
	input::InputSystem *InputSystemInstance = nullptr;
	threading::TaskScheduler *TaskSchedulerInstance = nullptr;
	threading::RenderThread *RenderThreadInstance = nullptr;
	diagnostics::DiagnosticSink *DiagnosticsInstance = nullptr;
};
} // namespace core
