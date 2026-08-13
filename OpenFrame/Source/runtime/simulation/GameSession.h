#pragma once

#include "Source/core/EngineAPI.h"
#include "Source/core/threading/TaskScheduler.h"
#include "Source/resource/asset/AssetManager.h"
#include "Source/runtime/behavior/BehaviorRuntime.h"
#include "Source/runtime/behavior/BehaviorRegistry.h"
#include "Source/scene/Scene.h"

#include <memory>
#include <stdexcept>

namespace runtime::simulation
{
struct GameSessionSpecification final
{
	float64 FixedDeltaSeconds = 1.0 / 60.0;
	float64 MaximumFrameDeltaSeconds = 0.25;
	uint32 MaximumFixedStepsPerFrame = 8;
};

class ENGINE_API GameSessionException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API GameSession final
{
  public:
	GameSession(std::unique_ptr<world::Scene> Scene, resource::AssetManager &Assets, const behavior::BehaviorRegistry &BehaviorRegistry,
				GameSessionSpecification Specification = {});
	~GameSession();

	GameSession(const GameSession &) = delete;
	GameSession &operator=(const GameSession &) = delete;
	GameSession(GameSession &&) = delete;
	GameSession &operator=(GameSession &&) = delete;

	void Start();
	void Tick(core::threading::TaskScheduler &Scheduler, float64 DeltaSeconds);
	void Stop();
	[[nodiscard]] world::Scene &GetScene() noexcept;
	[[nodiscard]] const world::Scene &GetScene() const noexcept;
	[[nodiscard]] bool IsRunning() const noexcept;

  private:
	GameSessionSpecification Specification;
	std::unique_ptr<world::Scene> Scene;
	std::unique_ptr<behavior::BehaviorRuntime> Behaviors;
	resource::AssetManager *Assets = nullptr;
	const behavior::BehaviorRegistry *BehaviorRegistry = nullptr;
	float64 FixedAccumulatorSeconds = 0.0;
	bool Running = false;
};
} // namespace runtime::simulation
