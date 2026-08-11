#include "PlaySession.h"

#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/editor/commands/CommandHistory.h"
#include "src/editor/document/SceneDocument.h"
#include "src/resource/asset/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace editor::play
{
namespace
{
struct AppliedTransform final
{
	glm::vec3 Position{0.0f};
	glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 Scale{1.0f};
};

[[nodiscard]] AppliedTransform ReadTransform(const components::CObjectTransformComponent &Transform)
{
	return {.Position = Transform.GetPosition(), .Rotation = Transform.GetRotation(), .Scale = Transform.GetScale()};
}

[[nodiscard]] bool EqualTransform(const AppliedTransform &Left, const AppliedTransform &Right) noexcept
{
	return glm::all(glm::equal(Left.Position, Right.Position)) && glm::all(glm::equal(Left.Scale, Right.Scale)) &&
		   Left.Rotation == Right.Rotation;
}

class ApplyRuntimeTransformCommand final : public commands::EditorCommand
{
  public:
	ApplyRuntimeTransformCommand(world::Scene &Scene, util::UUID ObjectID, AppliedTransform Before, AppliedTransform After)
		: Scene(&Scene), ObjectID(ObjectID), Before(Before), After(After)
	{
	}

	[[nodiscard]] string_view GetName() const noexcept override
	{
		return "Apply Runtime Transform";
	}

	void Execute() override
	{
		this->Apply(this->After);
	}

	void Undo() override
	{
		this->Apply(this->Before);
	}

  private:
	void Apply(const AppliedTransform &Value)
	{
		const world::ObjectHandle Object = this->Scene->FindObject(this->ObjectID);
		if (!Object.IsValid())
			throw std::out_of_range("Apply-back target no longer exists");
		auto Access = this->Scene->Write();
		const auto Handle = Access.GetComponent<components::CObjectTransformComponent>(Object);
		if (!Handle.IsValid())
			throw std::out_of_range("Apply-back target no longer has a transform component");
		Access.Resolve(Handle).SetTransform(Value.Position, Value.Rotation, Value.Scale);
	}

	world::Scene *Scene = nullptr;
	util::UUID ObjectID;
	AppliedTransform Before;
	AppliedTransform After;
};
} // namespace

PlaySession::PlaySession(resource::AssetManager &Assets, const runtime::behavior::BehaviorRegistry &BehaviorRegistry,
						 PlaySessionSpecification Specification)
	: Assets(&Assets), BehaviorRegistry(&BehaviorRegistry), Specification(Specification)
{
	this->OwnerThread = std::this_thread::get_id();
	this->ValidateSpecification();
}

PlaySession::~PlaySession()
{
	if (this->OwnerThread != std::this_thread::get_id())
		std::terminate();
	this->FinalizeRuntimeNoThrow();
}

void PlaySession::Start(const world::Scene &EditScene, const PlaySessionMode Mode)
{
	this->RequireOwnerThread();
	if (this->State != PlaySessionState::Stopped)
		throw PlaySessionException("Play session can only start from the stopped state");
	this->State = PlaySessionState::Starting;
	this->Diagnostic.clear();
	this->Statistics = {};
	this->FixedAccumulatorSeconds = 0.0;
	this->Mode = Mode;
	try
	{
		this->AuthoringTransforms.clear();
		{
			const auto Access = EditScene.Read();
			this->AuthoringTransforms.reserve(Access.Objects().size());
			for (const world::ObjectHandle Object : Access.Objects())
			{
				const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Object);
				const auto Transform = Access.GetComponent<components::CObjectTransformComponent>(Object);
				if (!Identity.IsValid() || !Transform.IsValid())
					continue;
				const AppliedTransform Value = ReadTransform(Access.Resolve(Transform));
				this->AuthoringTransforms.emplace(
					Access.Resolve(Identity).GetPersistentID(),
					TransformSnapshot{.Position = Value.Position, .Rotation = Value.Rotation, .Scale = Value.Scale});
			}
		}
		this->RuntimeWorld = world::SceneCloner::Clone(EditScene);
		if (this->Mode == PlaySessionMode::Play)
		{
			this->Behaviors = std::make_unique<runtime::behavior::BehaviorRuntime>(*this->RuntimeWorld.ClonedScene, *this->Assets,
																				   *this->BehaviorRegistry);
			this->Behaviors->Start();
			this->Statistics.Behaviors = this->Behaviors->GetStatistics();
		}
		this->State = PlaySessionState::Playing;
	}
	catch (...)
	{
		this->FailFromCurrentException("Could not start play session");
		this->FinalizeRuntimeNoThrow();
		throw;
	}
}

PlaySessionApplyBackResult PlaySession::ApplyBack(document::SceneDocument &Document, const PlaySessionApplyBackOptions Options)
{
	this->RequireOwnerThread();
	if (this->State != PlaySessionState::Paused || this->RuntimeWorld.ClonedScene == nullptr)
		throw PlaySessionException("Apply-back requires a paused session with a runtime scene");
	if (!Options.Transforms)
		return {};

	struct PendingChange final
	{
		util::UUID ObjectID;
		AppliedTransform Before;
		AppliedTransform After;
	};
	std::vector<PendingChange> Changes;
	PlaySessionApplyBackResult Result;
	const auto RuntimeAccess = this->RuntimeWorld.ClonedScene->Read();
	for (const world::ObjectHandle RuntimeObject : RuntimeAccess.Objects())
	{
		const auto RuntimeIdentity = RuntimeAccess.GetComponent<components::CObjectIdentityComponent>(RuntimeObject);
		const auto RuntimeTransform = RuntimeAccess.GetComponent<components::CObjectTransformComponent>(RuntimeObject);
		if (!RuntimeIdentity.IsValid() || !RuntimeTransform.IsValid())
			continue;
		const util::UUID ObjectID = RuntimeAccess.Resolve(RuntimeIdentity).GetPersistentID();
		const auto Baseline = this->AuthoringTransforms.find(ObjectID);
		const world::ObjectHandle EditObject = Document.GetScene().FindObject(ObjectID);
		if (Baseline == this->AuthoringTransforms.end() || !EditObject.IsValid())
		{
			++Result.SkippedObjects;
			continue;
		}
		const auto EditAccess = Document.GetScene().Read();
		const auto EditTransformHandle = EditAccess.GetComponent<components::CObjectTransformComponent>(EditObject);
		if (!EditTransformHandle.IsValid())
		{
			++Result.SkippedObjects;
			continue;
		}
		const AppliedTransform Original{
			.Position = Baseline->second.Position, .Rotation = Baseline->second.Rotation, .Scale = Baseline->second.Scale};
		const AppliedTransform Before = ReadTransform(EditAccess.Resolve(EditTransformHandle));
		const AppliedTransform After = ReadTransform(RuntimeAccess.Resolve(RuntimeTransform));
		if (EqualTransform(After, Original))
			continue;
		if (Options.ConflictPolicy == ApplyBackConflictPolicy::RejectAuthoringChanges && !EqualTransform(Before, Original))
			throw PlaySessionException("Apply-back detected an authoring transform conflict for object " + ObjectID.ToString());
		Changes.push_back({.ObjectID = ObjectID, .Before = Before, .After = After});
	}

	commands::EditorTransaction Transaction(Document.GetHistory(), "Apply Runtime Changes");
	try
	{
		for (const PendingChange &Change : Changes)
			Document.GetHistory().Execute(
				std::make_unique<ApplyRuntimeTransformCommand>(Document.GetScene(), Change.ObjectID, Change.Before, Change.After));
		Transaction.Commit();
		Result.ChangedObjects = Changes.size();
		return Result;
	}
	catch (...)
	{
		Transaction.Cancel();
		throw;
	}
}

void PlaySession::Tick(core::threading::TaskScheduler &Scheduler, const float64 DeltaSeconds)
{
	this->RequireOwnerThread();
	if (this->State == PlaySessionState::Paused)
		return;
	if (this->State != PlaySessionState::Playing)
		throw PlaySessionException("Play session must be playing before it can tick");
	if (!(DeltaSeconds >= 0.0) || !std::isfinite(DeltaSeconds))
		throw std::invalid_argument("Play-session delta must be finite and non-negative");
	try
	{
		this->RunSimulationStep(Scheduler, std::min(DeltaSeconds, this->Specification.MaximumFrameDeltaSeconds), false);
	}
	catch (...)
	{
		this->FailFromCurrentException("Play-session simulation failed");
		this->FinalizeRuntimeNoThrow();
		throw;
	}
}

void PlaySession::Pause()
{
	this->RequireOwnerThread();
	if (this->State != PlaySessionState::Playing)
		throw PlaySessionException("Only a playing session can be paused");
	this->State = PlaySessionState::Paused;
}

void PlaySession::Resume()
{
	this->RequireOwnerThread();
	if (this->State != PlaySessionState::Paused)
		throw PlaySessionException("Only a paused session can be resumed");
	this->State = PlaySessionState::Playing;
}

void PlaySession::Step(core::threading::TaskScheduler &Scheduler)
{
	this->RequireOwnerThread();
	if (this->State != PlaySessionState::Paused)
		throw PlaySessionException("Single-step requires a paused play session");
	try
	{
		this->RunSimulationStep(Scheduler, this->Specification.FixedDeltaSeconds, true);
	}
	catch (...)
	{
		this->FailFromCurrentException("Play-session single-step failed");
		this->FinalizeRuntimeNoThrow();
		throw;
	}
}

void PlaySession::Stop()
{
	this->RequireOwnerThread();
	if (this->State == PlaySessionState::Stopped)
		return;
	this->State = PlaySessionState::Stopping;
	std::exception_ptr Failure;
	try
	{
		if (this->Behaviors != nullptr)
			this->Behaviors->Stop();
	}
	catch (...)
	{
		Failure = std::current_exception();
		this->FailFromCurrentException("Could not stop play session cleanly");
	}
	this->ResetRuntime();
	if (Failure != nullptr)
		std::rethrow_exception(Failure);
	this->Diagnostic.clear();
	this->State = PlaySessionState::Stopped;
}

std::vector<runtime::behavior::BehaviorStateSnapshot> PlaySession::SuspendBehaviorsForReload()
{
	this->RequireOwnerThread();
	if (this->Mode != PlaySessionMode::Play || this->Behaviors == nullptr ||
		(this->State != PlaySessionState::Playing && this->State != PlaySessionState::Paused))
	{
		throw PlaySessionException("Only an active Play session can suspend behaviors for module reload");
	}
	this->StateBeforeReload = this->State;
	this->State = PlaySessionState::Quiescing;
	std::vector<runtime::behavior::BehaviorStateSnapshot> State = this->Behaviors->CaptureState();
	try
	{
		this->Behaviors->Stop();
		this->Behaviors.reset();
		this->State = PlaySessionState::Reloading;
	}
	catch (...)
	{
		this->FinalizeRuntimeNoThrow();
		this->State = PlaySessionState::Failed;
		throw;
	}
	return State;
}

void PlaySession::RestoreBehaviorsAfterReload(const std::span<const runtime::behavior::BehaviorStateSnapshot> State)
{
	this->RequireOwnerThread();
	if (this->Mode != PlaySessionMode::Play || this->State != PlaySessionState::Reloading || this->RuntimeWorld.ClonedScene == nullptr ||
		this->Behaviors != nullptr)
		throw PlaySessionException("Play session is not ready to restore behaviors after module reload");
	try
	{
		auto Replacement =
			std::make_unique<runtime::behavior::BehaviorRuntime>(*this->RuntimeWorld.ClonedScene, *this->Assets, *this->BehaviorRegistry);
		Replacement->Start(State);
		this->Behaviors = std::move(Replacement);
		this->Statistics.Behaviors = this->Behaviors->GetStatistics();
		this->State = this->StateBeforeReload;
		this->StateBeforeReload = PlaySessionState::Stopped;
	}
	catch (...)
	{
		this->FailFromCurrentException("Could not restore behaviors after module reload");
		this->FinalizeRuntimeNoThrow();
		throw;
	}
}

PlaySessionState PlaySession::GetState() const
{
	this->RequireOwnerThread();
	return this->State;
}

PlaySessionMode PlaySession::GetMode() const
{
	this->RequireOwnerThread();
	return this->Mode;
}

bool PlaySession::HasRuntimeScene() const
{
	this->RequireOwnerThread();
	return this->RuntimeWorld.ClonedScene != nullptr;
}

world::Scene *PlaySession::GetRuntimeScene()
{
	this->RequireOwnerThread();
	return this->RuntimeWorld.ClonedScene.get();
}

const world::Scene *PlaySession::GetRuntimeScene() const
{
	this->RequireOwnerThread();
	return this->RuntimeWorld.ClonedScene.get();
}

world::ObjectHandle PlaySession::FindRuntimeObject(const util::UUID &PersistentID) const
{
	this->RequireOwnerThread();
	return this->RuntimeWorld.FindObject(PersistentID);
}

PlaySessionStatistics PlaySession::GetStatistics() const
{
	this->RequireOwnerThread();
	PlaySessionStatistics Result = this->Statistics;
	if (this->Behaviors != nullptr)
		Result.Behaviors = this->Behaviors->GetStatistics();
	return Result;
}

const string &PlaySession::GetDiagnostic() const
{
	this->RequireOwnerThread();
	return this->Diagnostic;
}

void PlaySession::ValidateSpecification() const
{
	if (!(this->Specification.FixedDeltaSeconds > 0.0) || !std::isfinite(this->Specification.FixedDeltaSeconds))
		throw std::invalid_argument("Play-session fixed delta must be finite and positive");
	if (!(this->Specification.MaximumFrameDeltaSeconds >= this->Specification.FixedDeltaSeconds) ||
		!std::isfinite(this->Specification.MaximumFrameDeltaSeconds))
	{
		throw std::invalid_argument("Play-session maximum frame delta must be finite and at least one fixed step");
	}
	if (this->Specification.MaximumFixedStepsPerFrame == 0)
		throw std::invalid_argument("Play session must allow at least one fixed step per frame");
}

void PlaySession::RunSimulationStep(core::threading::TaskScheduler &Scheduler, const float64 DeltaSeconds, const bool ForceFixedStep)
{
	if (this->RuntimeWorld.ClonedScene == nullptr)
		throw PlaySessionException("Play session has no isolated runtime scene");

	this->FixedAccumulatorSeconds += DeltaSeconds;
	uint32 FixedSteps = 0;
	while (this->FixedAccumulatorSeconds >= this->Specification.FixedDeltaSeconds &&
		   FixedSteps < this->Specification.MaximumFixedStepsPerFrame)
	{
		if (this->Specification.FixedSimulation)
			this->Specification.FixedSimulation(*this->RuntimeWorld.ClonedScene, Scheduler, this->Specification.FixedDeltaSeconds);
		if (this->Behaviors != nullptr)
			this->Behaviors->FixedUpdate(Scheduler, static_cast<float32>(this->Specification.FixedDeltaSeconds));
		this->FixedAccumulatorSeconds -= this->Specification.FixedDeltaSeconds;
		++FixedSteps;
		++this->Statistics.FixedUpdates;
	}
	if (ForceFixedStep && FixedSteps == 0)
	{
		if (this->Specification.FixedSimulation)
			this->Specification.FixedSimulation(*this->RuntimeWorld.ClonedScene, Scheduler, this->Specification.FixedDeltaSeconds);
		if (this->Behaviors != nullptr)
			this->Behaviors->FixedUpdate(Scheduler, static_cast<float32>(this->Specification.FixedDeltaSeconds));
		++this->Statistics.FixedUpdates;
	}
	if (FixedSteps == this->Specification.MaximumFixedStepsPerFrame &&
		this->FixedAccumulatorSeconds >= this->Specification.FixedDeltaSeconds)
	{
		const float64 Retained = std::fmod(this->FixedAccumulatorSeconds, this->Specification.FixedDeltaSeconds);
		this->Statistics.DroppedFixedTimeSeconds += this->FixedAccumulatorSeconds - Retained;
		this->FixedAccumulatorSeconds = Retained;
	}

	if (this->Behaviors != nullptr)
	{
		this->Behaviors->Update(Scheduler, static_cast<float32>(DeltaSeconds));
		this->Statistics.Behaviors = this->Behaviors->GetStatistics();
	}
	if (this->Specification.VariableSimulation)
		this->Specification.VariableSimulation(*this->RuntimeWorld.ClonedScene, Scheduler, DeltaSeconds);
	++this->Statistics.VariableUpdates;
}

void PlaySession::FailFromCurrentException(const string_view Prefix)
{
	this->State = PlaySessionState::Failed;
	try
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		this->Diagnostic = string(Prefix) + ": " + Exception.what();
	}
	catch (...)
	{
		this->Diagnostic = string(Prefix) + ": non-standard exception";
	}
}

void PlaySession::ResetRuntime() noexcept
{
	this->Behaviors.reset();
	this->RuntimeWorld = {};
	this->AuthoringTransforms.clear();
	this->FixedAccumulatorSeconds = 0.0;
	if (this->State == PlaySessionState::Stopping)
		this->State = PlaySessionState::Stopped;
}

void PlaySession::FinalizeRuntimeNoThrow() noexcept
{
	if (this->Behaviors != nullptr)
	{
		try
		{
			this->Behaviors->Stop();
		}
		catch (...)
		{
		}
	}
	this->ResetRuntime();
}

void PlaySession::RequireOwnerThread() const
{
	if (this->OwnerThread != std::this_thread::get_id())
		throw PlaySessionException("PlaySession operation executed outside its owner thread");
}
} // namespace editor::play
