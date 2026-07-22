#pragma once

#include "src/types.h"

#include <chrono>

namespace core
{
struct FrameTiming final
{
	uint64 FrameNumber = 0;
	float64 DeltaSeconds = 0.0;
	float64 ElapsedSeconds = 0.0;
};

class FrameClock final
{
  public:
	explicit FrameClock(float64 MaximumDeltaSeconds = 0.25);
	[[nodiscard]] FrameTiming Tick();
	void Reset() noexcept;
	[[nodiscard]] const FrameTiming &GetTiming() const noexcept;

  private:
	using Clock = std::chrono::steady_clock;
	Clock::time_point Start;
	Clock::time_point Previous;
	FrameTiming Timing;
	float64 MaximumDeltaSeconds = 0.25;
	bool Initialized = false;
};
} // namespace core
