#include "ApplicationServices.h"

#include "Application.h"
#include "Source/core/diagnostics/DiagnosticSink.h"
#include "Source/core/input/InputSystem.h"
#include "Source/core/threading/RenderThread.h"
#include "Source/core/threading/TaskScheduler.h"
#include "Source/core/window/WindowManager.h"

namespace core
{
ApplicationServices::ApplicationServices(Application &Application, WindowManager &WindowManager, input::InputSystem &InputSystem,
										 threading::TaskScheduler &TaskScheduler, threading::RenderThread &RenderThread,
										 diagnostics::DiagnosticSink &Diagnostics) noexcept
	: ApplicationInstance(&Application), WindowManagerInstance(&WindowManager), InputSystemInstance(&InputSystem),
	  TaskSchedulerInstance(&TaskScheduler), RenderThreadInstance(&RenderThread), DiagnosticsInstance(&Diagnostics)
{
}

Window &ApplicationServices::GetPrimaryWindow() const
{
	this->RequireApplicationThread();
	return this->WindowManagerInstance->GetPrimaryWindow();
}

WindowManager &ApplicationServices::GetWindowManager() const
{
	this->RequireApplicationThread();
	return *this->WindowManagerInstance;
}

input::InputSystem &ApplicationServices::GetInputSystem() const
{
	this->RequireApplicationThread();
	return *this->InputSystemInstance;
}

threading::TaskScheduler &ApplicationServices::GetTaskScheduler() const noexcept
{
	return *this->TaskSchedulerInstance;
}

threading::RenderThread &ApplicationServices::GetRenderThread() const noexcept
{
	return *this->RenderThreadInstance;
}

diagnostics::DiagnosticSink &ApplicationServices::GetDiagnostics() const noexcept
{
	return *this->DiagnosticsInstance;
}

void ApplicationServices::RequestStop() const noexcept
{
	this->ApplicationInstance->Stop();
}

void ApplicationServices::RequireApplicationThread() const
{
	this->ApplicationInstance->RequireApplicationThread();
}

bool ApplicationServices::IsApplicationThread() const noexcept
{
	return this->ApplicationInstance->IsApplicationThread();
}
} // namespace core
