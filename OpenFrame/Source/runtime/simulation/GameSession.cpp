#include "GameSession.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace runtime::simulation
{
GameSession::GameSession(std::unique_ptr<world::Scene> Scene, resource::AssetManager &Assets,
						 const behavior::BehaviorRegistry &BehaviorRegistry, const GameSessionSpecification Specification)
	: Specification(Specification), Scene(std::move(Scene)), Assets(&Assets), BehaviorRegistry(&BehaviorRegistry)
{
	if (this->Scene == nullptr)
		throw std::invalid_argument("GameSession requires an owned scene");
	if (!(this->Specification.FixedDeltaSeconds > 0.0) || !std::isfinite(this->Specification.FixedDeltaSeconds) ||
		!(this->Specification.MaximumFrameDeltaSeconds >= this->Specification.FixedDeltaSeconds) ||
		!std::isfinite(this->Specification.MaximumFrameDeltaSeconds) || this->Specification.MaximumFixedStepsPerFrame == 0)
	{
		throw std::invalid_argument("GameSession timing specification is invalid");
	}
}

GameSession::~GameSession()
{
	try
	{
		this->Stop();
	}
	catch (...)
	{
		std::terminate();
	}
}

void GameSession::Start()
{
	if (this->Running)
		throw GameSessionException("GameSession is already running");
	this->Behaviors = std::make_unique<behavior::BehaviorRuntime>(*this->Scene, *this->Assets, *this->BehaviorRegistry);
	this->Behaviors->Start();
	this->FixedAccumulatorSeconds = 0.0;
	this->Running = true;
}

void GameSession::Tick(core::threading::TaskScheduler &Scheduler, const float64 DeltaSeconds)
{
	if (!this->Running || this->Behaviors == nullptr)
		throw GameSessionException("GameSession must be started before it can tick");
	if (!(DeltaSeconds >= 0.0) || !std::isfinite(DeltaSeconds))
		throw std::invalid_argument("GameSession delta must be finite and non-negative");

	const float64 ClampedDelta = std::min(DeltaSeconds, this->Specification.MaximumFrameDeltaSeconds);
	this->FixedAccumulatorSeconds += ClampedDelta;
	uint32 FixedSteps = 0;
	while (this->FixedAccumulatorSeconds >= this->Specification.FixedDeltaSeconds &&
		   FixedSteps < this->Specification.MaximumFixedStepsPerFrame)
	{
		this->Behaviors->FixedUpdate(Scheduler, static_cast<float32>(this->Specification.FixedDeltaSeconds));
		this->FixedAccumulatorSeconds -= this->Specification.FixedDeltaSeconds;
		++FixedSteps;
	}
	if (FixedSteps == this->Specification.MaximumFixedStepsPerFrame &&
		this->FixedAccumulatorSeconds >= this->Specification.FixedDeltaSeconds)
	{
		this->FixedAccumulatorSeconds = std::fmod(this->FixedAccumulatorSeconds, this->Specification.FixedDeltaSeconds);
	}
	this->Behaviors->Update(Scheduler, static_cast<float32>(ClampedDelta));
}

void GameSession::Stop()
{
	if (!this->Running)
		return;
	if (this->Behaviors != nullptr)
		this->Behaviors->Stop();
	this->Behaviors.reset();
	this->FixedAccumulatorSeconds = 0.0;
	this->Running = false;
}

world::Scene &GameSession::GetScene() noexcept
{
	return *this->Scene;
}

const world::Scene &GameSession::GetScene() const noexcept
{
	return *this->Scene;
}

bool GameSession::IsRunning() const noexcept
{
	return this->Running;
}
} // namespace runtime::simulation
