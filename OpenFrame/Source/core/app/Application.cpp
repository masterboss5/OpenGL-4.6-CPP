#include "Application.h"

#include "Source/pipeline/device/Device.h"

#include <GL/glew.h>
#include <exception>
#include <stdexcept>

namespace core
{
Application::Application(ApplicationSpecification Specification)
	: Services(*this, this->WindowManager, this->InputSystem, this->TaskScheduler, this->RenderThread, this->Diagnostics),
	  MaximumFrameCount(Specification.MaximumFrameCount), ApplicationThreadID(std::this_thread::get_id())
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
	this->RequireApplicationThread();
	this->Running.store(true, std::memory_order_release);
	uint64 FrameCount = 0;
	try
	{
		while (this->Running.load(std::memory_order_acquire))
		{
			this->CommitPendingLayers();
			const FrameTiming Timing = this->FrameClock.Tick();
			this->WindowManager.PollEvents();
			this->InputSystem.BeginFrame(this->WindowManager);
			this->WindowManager.ConsumeEventsInto(this->WindowEventsScratch);
			for (const WindowEvent &Event : this->WindowEventsScratch)
			{
				if (Event.Window == this->Window->GetID() && Event.Type == WindowEventType::CloseRequested)
					this->Stop();
			}
			if (this->Window->ShouldClose())
				this->Stop();
			if (!this->Running.load(std::memory_order_acquire))
				break;

			const ApplicationFrame Frame(Timing, this->Window->GetID(), this->Window->GetFramebufferExtent(),
										 this->InputSystem.GetSnapshot(this->Window->GetID()), this->WindowEventsScratch, this->Services);

			this->DispatchingLayers = true;
			for (const auto &Layer : this->Layers)
			{
				Layer->Run(Frame);
				if (!this->Running.load(std::memory_order_acquire))
					break;
			}
			this->DispatchingLayers = false;

			++FrameCount;
			if (this->MaximumFrameCount != 0 && FrameCount >= this->MaximumFrameCount)
				this->Stop();
		}
	}
	catch (const std::exception &Exception)
	{
		this->DispatchingLayers = false;
		this->Stop();
		try
		{
			this->Diagnostics.Publish(diagnostics::DiagnosticSeverity::Fatal, "Application", Exception.what());
		}
		catch (...)
		{
		}
		throw;
	}
	catch (...)
	{
		this->DispatchingLayers = false;
		this->Stop();
		try
		{
			this->Diagnostics.Publish(diagnostics::DiagnosticSeverity::Fatal, "Application", "Unknown application-layer exception");
		}
		catch (...)
		{
		}
		throw;
	}
}

void Application::Stop()
{
	this->Running.store(false, std::memory_order_release);
}

usize Application::GetLayerCount() const
{
	this->RequireApplicationThread();
	return this->Layers.size() + this->PendingLayers.size();
}

void Application::RequireApplicationThread() const
{
	if (std::this_thread::get_id() != this->ApplicationThreadID)
		throw std::logic_error("Application operation requires the application thread");
}

bool Application::IsApplicationThread() const noexcept
{
	return std::this_thread::get_id() == this->ApplicationThreadID;
}

void Application::QueueLayer(std::unique_ptr<ApplicationLayer> Layer)
{
	if (this->DispatchingLayers)
		this->PendingLayers.push_back(std::move(Layer));
	else
		this->Layers.push_back(std::move(Layer));
}

void Application::CommitPendingLayers()
{
	if (this->PendingLayers.empty())
		return;
	this->Layers.reserve(this->Layers.size() + this->PendingLayers.size());
	for (std::unique_ptr<ApplicationLayer> &Layer : this->PendingLayers)
		this->Layers.push_back(std::move(Layer));
	this->PendingLayers.clear();
}

Window &Application::GetPrimaryWindow() const
{
	this->RequireApplicationThread();
	return *this->Window;
}
WindowManager &Application::GetWindowManager()
{
	this->RequireApplicationThread();
	return this->WindowManager;
}
input::InputSystem &Application::GetInputSystem()
{
	this->RequireApplicationThread();
	return this->InputSystem;
}
} // namespace core
