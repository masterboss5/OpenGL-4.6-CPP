#pragma once

#include "src/core/EngineAPI.h"

#include "BehaviorRegistry.h"
#include "src/core/threading/TaskScheduler.h"
#include "src/scene/SceneCommandBuffer.h"

#include <memory>
#include <span>
#include <vector>

namespace runtime::behavior
{
class ENGINE_API BehaviorRuntimeException : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API BehaviorResolutionException final : public BehaviorRuntimeException
{
  public:
	using BehaviorRuntimeException::BehaviorRuntimeException;
};

class ENGINE_API BehaviorLifecycleException final : public BehaviorRuntimeException
{
  public:
	using BehaviorRuntimeException::BehaviorRuntimeException;
};

struct BehaviorRuntimeStatistics final
{
	uint32 RegisteredInstances = 0;
	uint32 ActiveInstances = 0;
	uint32 FailedInstances = 0;
	uint64 ParallelCallbacks = 0;
	uint64 SerialCallbacks = 0;
};

struct BehaviorStateSnapshot final
{
	util::UUID InstanceID;
	components::BehaviorTypeID Type = 0;
	uint32 SchemaVersion = 0;
	std::unordered_map<string, components::BehaviorPropertyValue> Properties;
	bool HasRuntimeState = false;
	std::vector<std::byte> Data;
};

class ENGINE_API BehaviorRuntime final
{
  public:
	BehaviorRuntime(world::Scene &Scene, resource::AssetManager &Assets, const BehaviorRegistry &Registry);
	~BehaviorRuntime();

	BehaviorRuntime(const BehaviorRuntime &) = delete;
	BehaviorRuntime &operator=(const BehaviorRuntime &) = delete;
	BehaviorRuntime(BehaviorRuntime &&) = delete;
	BehaviorRuntime &operator=(BehaviorRuntime &&) = delete;

	void Start(std::span<const BehaviorStateSnapshot> State = {});
	void Update(core::threading::TaskScheduler &Scheduler, float32 DeltaSeconds);
	void FixedUpdate(core::threading::TaskScheduler &Scheduler, float32 FixedDeltaSeconds);
	void Stop();
	[[nodiscard]] std::vector<BehaviorStateSnapshot> CaptureState() const;

	[[nodiscard]] bool IsRunning() const noexcept;
	[[nodiscard]] BehaviorRuntimeStatistics GetStatistics() const noexcept;

  private:
	enum class CallbackPhase : uint8
	{
		Update,
		FixedUpdate
	};

	struct RuntimeEntry final
	{
		world::ObjectHandle Owner;
		world::ComponentHandle<components::CObjectBehaviorComponent> Component;
		util::UUID InstanceID;
		BehaviorDescriptor Descriptor;
		std::unordered_map<string, components::BehaviorPropertyValue> Properties;
		usize StateOffset = 0;
		bool Constructed = false;
		bool Started = false;
		bool Failed = false;
		string Failure;
		std::unique_ptr<world::SceneCommandBuffer> StagedCommands;
	};

	void BuildEntries(std::span<const BehaviorStateSnapshot> State);
	void AllocateStorage();
	void ConstructAndStart(std::span<const BehaviorStateSnapshot> State);
	void RunPhase(core::threading::TaskScheduler &Scheduler, float32 DeltaSeconds, CallbackPhase Phase);
	void InvokePhase(RuntimeEntry &Entry, float32 DeltaSeconds, CallbackPhase Phase);
	void RetireFailedEntries();
	void ExecutePendingCommands();
	void ReleaseEntries(bool PreserveFailures) noexcept;
	void SetInstanceState(const RuntimeEntry &Entry, components::BehaviorExecutionState State, string Diagnostic = {}) noexcept;
	[[nodiscard]] void *GetState(const RuntimeEntry &Entry) const noexcept;
	[[nodiscard]] const void *GetStateConst(const RuntimeEntry &Entry) const noexcept;
	[[nodiscard]] static string CurrentExceptionDiagnostic(string_view Prefix) noexcept;

	world::Scene *Scene = nullptr;
	resource::AssetManager *Assets = nullptr;
	const BehaviorRegistry *Registry = nullptr;
	std::vector<RuntimeEntry> Entries;
	world::SceneCommandBuffer PendingCommands;
	void *Storage = nullptr;
	usize StorageSize = 0;
	usize StorageAlignment = alignof(std::max_align_t);
	bool Running = false;
	BehaviorRuntimeStatistics Statistics;
};
} // namespace runtime::behavior
