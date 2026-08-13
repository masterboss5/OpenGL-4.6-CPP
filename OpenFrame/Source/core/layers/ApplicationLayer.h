#pragma once

#include "Source/core/input/InputTypes.h"
#include "Source/core/time/FrameClock.h"
#include "Source/core/window/WindowTypes.h"

#include <span>

namespace core
{
class ApplicationServices;

struct ApplicationFrame final
{
	ApplicationFrame(FrameTiming Timing, WindowID Window, WindowExtent FramebufferExtent, const input::InputSnapshot &Input,
					 std::span<const WindowEvent> WindowEvents, ApplicationServices &Services) noexcept
		: Timing(Timing), Window(Window), FramebufferExtent(FramebufferExtent), Input(Input), WindowEvents(WindowEvents), Services(Services)
	{
	}

	ApplicationFrame(const ApplicationFrame &) = delete;
	ApplicationFrame &operator=(const ApplicationFrame &) = delete;
	ApplicationFrame(ApplicationFrame &&) = delete;
	ApplicationFrame &operator=(ApplicationFrame &&) = delete;

	FrameTiming Timing;
	WindowID Window;
	WindowExtent FramebufferExtent;
	const input::InputSnapshot &Input;
	std::span<const WindowEvent> WindowEvents;
	ApplicationServices &Services;
};
} // namespace core

class ApplicationLayer
{
  public:
	virtual ~ApplicationLayer() = default;
	virtual void Run(const core::ApplicationFrame &Frame) = 0;
};
