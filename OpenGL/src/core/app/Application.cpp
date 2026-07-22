#include "Application.h"

#include "src/pipeline/device/Device.h"
#include "src/renderer/RenderCoreValidation.h"

#include <GL/glew.h>
#include <stdexcept>

namespace core
{
Application::Application(ApplicationSpecification Specification)
	: DeterministicRenderValidation(Specification.DeterministicRenderValidation)
{
	this->Window = &this->WindowManager.CreateWindow(Specification.Window);
	pipeline::device::Device &Device = this->WindowManager.GetDevice(*this->Window);
	(void)Device.RequireCurrentContext();
	glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
	glClearDepth(0.0);
	glDepthFunc(GL_GREATER);
	if (this->Window->IsSRGBPresentationCapable())
		glEnable(GL_FRAMEBUFFER_SRGB);
	else
		glDisable(GL_FRAMEBUFFER_SRGB);
	Device.CheckErrors("Application render convention initialization");
	if (this->DeterministicRenderValidation)
		renderer::validation::RunDeterministicRenderCoreChecks(Device);
	(void)this->Window->SetPresentationMode(PresentationMode::Off);
	this->Window->SetCursorMode(CursorMode::Relative);
}

Application::Application(WindowSpecification WindowSpecification)
	: Application(ApplicationSpecification{.Window = std::move(WindowSpecification)})
{
}

Application::~Application()
{
	this->Layers.clear();
}

void Application::Main()
{
	this->Running = true;
	while (this->Running)
	{
		const FrameTiming Timing = this->FrameClock.Tick();
		this->WindowManager.PollEvents();
		this->InputSystem.BeginFrame(this->WindowManager);
		for (const WindowEvent &Event : this->WindowManager.ConsumeEvents())
		{
			if (Event.Window == this->Window->GetID() && Event.Type == WindowEventType::CloseRequested)
				this->Running = false;
		}
		this->Running = this->Running && !this->Window->ShouldClose();
		if (!this->Running)
			break;

		pipeline::device::Device &Device = this->WindowManager.GetDevice(*this->Window);
		Device.ValidateStatus();
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_GREATER);
		Device.CheckErrors("Application frame initialization");

		const ApplicationFrame Frame{.Timing = Timing,
									 .Window = this->Window->GetID(),
									 .FramebufferExtent = this->Window->GetFramebufferExtent(),
									 .Input = &this->InputSystem.GetSnapshot(this->Window->GetID())};

		for (const auto &Layer : this->Layers)
		{
			Layer->Run(Frame);
		}

		this->Window->Present();
	}
}

void Application::Stop()
{
	this->Running = false;
}

usize Application::GetLayerCount() const
{
	return this->Layers.size();
}

Window &Application::GetPrimaryWindow() const
{
	return *this->Window;
}
WindowManager &Application::GetWindowManager() noexcept
{
	return this->WindowManager;
}
input::InputSystem &Application::GetInputSystem() noexcept
{
	return this->InputSystem;
}
} // namespace core
