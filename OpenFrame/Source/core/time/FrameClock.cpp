#include "FrameClock.h"

#include <algorithm>
#include <stdexcept>

namespace core
{
FrameClock::FrameClock(const float64 MaximumDeltaSeconds) : MaximumDeltaSeconds(MaximumDeltaSeconds)
{
	if (MaximumDeltaSeconds <= 0.0)
		throw std::invalid_argument("FrameClock maximum delta must be positive");
}

FrameTiming FrameClock::Tick()
{
	const Clock::time_point Now = Clock::now();
	if (!this->Initialized)
	{
		this->Start = Now;
		this->Previous = Now;
		this->Initialized = true;
		this->Timing = {};
		return this->Timing;
	}
	this->Timing.DeltaSeconds = std::min(std::chrono::duration<float64>(Now - this->Previous).count(), this->MaximumDeltaSeconds);
	this->Timing.ElapsedSeconds = std::chrono::duration<float64>(Now - this->Start).count();
	++this->Timing.FrameNumber;
	this->Previous = Now;
	return this->Timing;
}

void FrameClock::Reset() noexcept
{
	this->Initialized = false;
	this->Timing = {};
}
const FrameTiming &FrameClock::GetTiming() const noexcept
{
	return this->Timing;
}
} // namespace core
