#pragma once

#include "src/runtime/behavior/BehaviorRuntime.h"
#include "src/scene/SceneCloner.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace editor::document
{
class SceneDocument;
}

namespace editor::play
{
enum class PlaySessionMode : uint8
{
	Simulate,
	Play
};

enum class PlaySessionState : uint8
{
	Stopped,
	Starting,
	Playing,
	Paused,
	Quiescing,
	Reloading,
	Stopping,
	Failed
};

struct PlaySessionSpecification final
{
	using SimulationStep = std::function<void(world::Scene &, core::threading::TaskScheduler &, float64)>;

	float64 FixedDeltaSeconds = 1.0 / 60.0;
	float64 MaximumFrameDeltaSeconds = 0.25;
	uint32 MaximumFixedStepsPerFrame = 8;
	SimulationStep FixedSimulation;
	SimulationStep VariableSimulation;
};

struct PlaySessionStatistics final
{
	uint64 VariableUpdates = 0;
	uint64 FixedUpdates = 0;
	float64 DroppedFixedTimeSeconds = 0.0;
	runtime::behavior::BehaviorRuntimeStatistics Behaviors;
};

enum class ApplyBackConflictPolicy : uint8
{
	RejectAuthoringChanges,
	RuntimeWins
};

struct PlaySessionApplyBackOptions final
{
	bool Transforms = true;
	ApplyBackConflictPolicy ConflictPolicy = ApplyBackConflictPolicy::RejectAuthoringChanges;
};

struct PlaySessionApplyBackResult final
{
	usize ChangedObjects = 0;
	usize SkippedObjects = 0;
};

class PlaySessionException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class PlaySession final
{
  public:
	PlaySession(resource::AssetManager &Assets, const runtime::behavior::BehaviorRegistry &BehaviorRegistry,
				PlaySessionSpecification Specification = {});
	~PlaySession();

	PlaySession(const PlaySession &) = delete;
	PlaySession &operator=(const PlaySession &) = delete;
	PlaySession(PlaySession &&) = delete;
	PlaySession &operator=(PlaySession &&) = delete;

	void Start(const world::Scene &EditScene, PlaySessionMode Mode = PlaySessionMode::Play);
	void Tick(core::threading::TaskScheduler &Scheduler, float64 DeltaSeconds);
	void Pause();
	void Resume();
	void Step(core::threading::TaskScheduler &Scheduler);
	void Stop();
	[[nodiscard]] PlaySessionApplyBackResult ApplyBack(document::SceneDocument &Document, PlaySessionApplyBackOptions Options = {});
	[[nodiscard]] std::vector<runtime::behavior::BehaviorStateSnapshot> SuspendBehaviorsForReload();
	void RestoreBehaviorsAfterReload(std::span<const runtime::behavior::BehaviorStateSnapshot> State);

	[[nodiscard]] PlaySessionState GetState() const;
	[[nodiscard]] PlaySessionMode GetMode() const;
	[[nodiscard]] bool HasRuntimeScene() const;
	[[nodiscard]] world::Scene *GetRuntimeScene();
	[[nodiscard]] const world::Scene *GetRuntimeScene() const;
	[[nodiscard]] world::ObjectHandle FindRuntimeObject(const util::UUID &PersistentID) const;
	[[nodiscard]] PlaySessionStatistics GetStatistics() const;
	[[nodiscard]] const string &GetDiagnostic() const;

  private:
	void ValidateSpecification() const;
	void RunSimulationStep(core::threading::TaskScheduler &Scheduler, float64 DeltaSeconds, bool ForceFixedStep);
	void FailFromCurrentException(string_view Prefix);
	void FinalizeRuntimeNoThrow() noexcept;
	void ResetRuntime() noexcept;
	void RequireOwnerThread() const;
	struct TransformSnapshot final
	{
		glm::vec3 Position{0.0f};
		glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 Scale{1.0f};
	};

	resource::AssetManager *Assets = nullptr;
	const runtime::behavior::BehaviorRegistry *BehaviorRegistry = nullptr;
	PlaySessionSpecification Specification;
	PlaySessionMode Mode = PlaySessionMode::Play;
	PlaySessionState State = PlaySessionState::Stopped;
	PlaySessionState StateBeforeReload = PlaySessionState::Stopped;
	std::thread::id OwnerThread;
	world::SceneCloneResult RuntimeWorld;
	std::unique_ptr<runtime::behavior::BehaviorRuntime> Behaviors;
	float64 FixedAccumulatorSeconds = 0.0;
	PlaySessionStatistics Statistics;
	string Diagnostic;
	std::unordered_map<util::UUID, TransformSnapshot> AuthoringTransforms;
};
} // namespace editor::play

namespace editor
{
// PlaySession owns the cloned-world controller for editor simulation and play.
using EditorPlayController = play::PlaySession;
} // namespace editor
