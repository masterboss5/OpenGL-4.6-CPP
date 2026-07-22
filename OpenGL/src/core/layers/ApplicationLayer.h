#pragma once

#include "src/core/input/InputTypes.h"
#include "src/core/time/FrameClock.h"
#include "src/core/window/WindowTypes.h"

namespace core
{
struct ApplicationFrame final
{
	FrameTiming Timing;
	WindowID Window;
	WindowExtent FramebufferExtent;
	const input::InputSnapshot *Input = nullptr;
};
} // namespace core

class ApplicationLayer
{
  public:
	virtual ~ApplicationLayer() = default;
	virtual void Run(const core::ApplicationFrame &Frame) = 0;
};
