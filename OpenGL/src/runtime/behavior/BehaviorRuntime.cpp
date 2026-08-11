#include "BehaviorRuntime.h"

#include "src/resource/asset/AssetManager.h"
#include "src/scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace runtime::behavior
{
namespace
{
[[nodiscard]] usize AlignUp(const usize Value, const usize Alignment)
{
	const usize Mask = Alignment - 1U;
	if (Value > std::numeric_limits<usize>::max() - Mask)
		throw BehaviorLifecycleException("Behavior runtime state layout exceeds addressable memory");
	return (Value + Mask) & ~Mask;
}
} // namespace

BehaviorRuntime::BehaviorRuntime(world::Scene &Scene, resource::AssetManager &Assets, const BehaviorRegistry &Registry)
	: Scene(&Scene), Assets(&Assets), Registry(&Registry)
{
}

BehaviorRuntime::~BehaviorRuntime()
{
	this->ReleaseEntries(false);
}

void BehaviorRuntime::Start(const std::span<const BehaviorStateSnapshot> State)
{
	if (this->Running || !this->Entries.empty() || this->Storage != nullptr)
		throw BehaviorLifecycleException("Behavior runtime is already started");
	try
	{
		this->BuildEntries(State);
		this->AllocateStorage();
		this->ConstructAndStart(State);
		this->ExecutePendingCommands();
		this->Running = true;
	}
	catch (...)
	{
		this->PendingCommands.Clear();
		this->ReleaseEntries(true);
		throw;
	}
}

void BehaviorRuntime::Update(core::threading::TaskScheduler &Scheduler, const float32 DeltaSeconds)
{
	if (!this->Running)
		throw BehaviorLifecycleException("Behavior runtime must be running before update");
	if (!(DeltaSeconds >= 0.0f) || !std::isfinite(DeltaSeconds))
		throw std::invalid_argument("Behavior update delta must be finite and non-negative");
	this->RunPhase(Scheduler, DeltaSeconds, CallbackPhase::Update);
}

void BehaviorRuntime::FixedUpdate(core::threading::TaskScheduler &Scheduler, const float32 FixedDeltaSeconds)
{
	if (!this->Running)
		throw BehaviorLifecycleException("Behavior runtime must be running before fixed update");
	if (!(FixedDeltaSeconds > 0.0f) || !std::isfinite(FixedDeltaSeconds))
		throw std::invalid_argument("Behavior fixed-update delta must be finite and positive");
	this->RunPhase(Scheduler, FixedDeltaSeconds, CallbackPhase::FixedUpdate);
}

void BehaviorRuntime::Stop()
{
	if (!this->Running && this->Entries.empty() && this->Storage == nullptr)
		return;

	std::exception_ptr FirstFailure;
	for (usize Index = this->Entries.size(); Index-- > 0;)
	{
		RuntimeEntry &Entry = this->Entries[Index];
		if (!Entry.Started || Entry.Descriptor.Stop == nullptr)
			continue;
		try
		{
			world::SceneCommandBuffer LocalCommands;
			BehaviorExecutionContext Context(*this->Scene, LocalCommands, *this->Assets, Entry.Owner, Entry.InstanceID, Entry.Properties);
			Entry.Descriptor.Stop(this->GetState(Entry), &Context);
			this->PendingCommands.Absorb(LocalCommands);
		}
		catch (...)
		{
			if (FirstFailure == nullptr)
				FirstFailure = std::current_exception();
			Entry.Failed = true;
			Entry.Failure = BehaviorRuntime::CurrentExceptionDiagnostic("Behavior stop failed");
		}
		Entry.Started = false;
	}

	try
	{
		this->ExecutePendingCommands();
	}
	catch (...)
	{
		if (FirstFailure == nullptr)
			FirstFailure = std::current_exception();
	}
	this->ReleaseEntries(true);
	if (FirstFailure != nullptr)
		std::rethrow_exception(FirstFailure);
}

std::vector<BehaviorStateSnapshot> BehaviorRuntime::CaptureState() const
{
	if (!this->Running)
		throw BehaviorLifecycleException("Behavior runtime must be running before state capture");
	std::vector<BehaviorStateSnapshot> Result;
	Result.reserve(this->Entries.size());
	for (const RuntimeEntry &Entry : this->Entries)
	{
		BehaviorStateSnapshot Snapshot{.InstanceID = Entry.InstanceID,
									   .Type = Entry.Descriptor.Type,
									   .SchemaVersion = Entry.Descriptor.SchemaVersion,
									   .Properties = Entry.Properties};
		if (!Entry.Constructed || Entry.Failed)
		{
			Result.push_back(std::move(Snapshot));
			continue;
		}
		if (Entry.Descriptor.SerializeState == nullptr)
			throw BehaviorLifecycleException("Behavior '" + Entry.Descriptor.Name + "' does not support hot-reload state capture");
		std::array<char, 2'048> Diagnostic{};
		usize RequiredSize = 0;
		if (!Entry.Descriptor.SerializeState(this->GetStateConst(Entry), nullptr, 0, &RequiredSize, Diagnostic.data(), Diagnostic.size()))
		{
			throw BehaviorLifecycleException("Behavior '" + Entry.Descriptor.Name + "' state-size query failed: " +
											 (Diagnostic.front() == '\0' ? string("no diagnostic supplied") : string(Diagnostic.data())));
		}
		constexpr usize MaximumStateBytes = 64U * 1'024U * 1'024U;
		if (RequiredSize > MaximumStateBytes)
			throw BehaviorLifecycleException("Behavior '" + Entry.Descriptor.Name + "' state exceeds the 64 MiB reload limit");
		Snapshot.HasRuntimeState = true;
		Snapshot.Data.resize(RequiredSize);
		usize WrittenSize = RequiredSize;
		Diagnostic.fill('\0');
		if (!Entry.Descriptor.SerializeState(this->GetStateConst(Entry), Snapshot.Data.data(), Snapshot.Data.size(), &WrittenSize,
											 Diagnostic.data(), Diagnostic.size()) ||
			WrittenSize > Snapshot.Data.size())
		{
			throw BehaviorLifecycleException("Behavior '" + Entry.Descriptor.Name + "' state capture failed: " +
											 (Diagnostic.front() == '\0' ? string("invalid serialized size") : string(Diagnostic.data())));
		}
		Snapshot.Data.resize(WrittenSize);
		Result.push_back(std::move(Snapshot));
	}
	return Result;
}

bool BehaviorRuntime::IsRunning() const noexcept
{
	return this->Running;
}

BehaviorRuntimeStatistics BehaviorRuntime::GetStatistics() const noexcept
{
	return this->Statistics;
}

void BehaviorRuntime::BuildEntries(const std::span<const BehaviorStateSnapshot> State)
{
	const auto Access = this->Scene->Write();
	const std::span<components::CObjectBehaviorComponent> Components = Access.Components<components::CObjectBehaviorComponent>();
	usize InstanceCount = 0;
	for (const components::CObjectBehaviorComponent &Component : Components)
		InstanceCount += Component.GetBehaviors().size();
	this->Entries.reserve(InstanceCount);

	for (components::CObjectBehaviorComponent &Component : Components)
	{
		const world::ComponentHandle<components::CObjectBehaviorComponent> ComponentHandle =
			Access.GetComponent<components::CObjectBehaviorComponent>(Component.GetOwner());
		for (const components::BehaviorInstance &AttachedInstance : Component.GetBehaviors())
		{
			components::BehaviorInstance Instance = AttachedInstance;
			Component.SetExecutionState(Instance.InstanceID, components::BehaviorExecutionState::Unresolved);
			if (!Component.IsEnabled() || !Instance.Enabled)
			{
				Component.SetExecutionState(Instance.InstanceID, components::BehaviorExecutionState::Suspended);
				continue;
			}

			const std::optional<BehaviorDescriptor> Descriptor = this->Registry->Find(Instance.Type);
			if (!Descriptor.has_value())
			{
				Instance.Diagnostic = "No behavior descriptor is registered for type '" + Instance.TypeName + "'";
				Component.SetExecutionState(Instance.InstanceID, components::BehaviorExecutionState::Failed, Instance.Diagnostic);
				throw BehaviorResolutionException(Instance.Diagnostic);
			}
			if (Descriptor->Name != Instance.TypeName)
			{
				Instance.Diagnostic =
					"Behavior type identity resolves to '" + Descriptor->Name + "', not authored name '" + Instance.TypeName + "'";
				Component.SetExecutionState(Instance.InstanceID, components::BehaviorExecutionState::Failed, Instance.Diagnostic);
				throw BehaviorResolutionException(Instance.Diagnostic);
			}
			if (Descriptor->ModuleName != Instance.ModuleName || Descriptor->StableTypeID != Instance.StableTypeID)
			{
				Instance.Diagnostic = "Behavior stable identity does not match the registered module-qualified type '" +
									  Descriptor->ModuleName + "::" + Descriptor->Name + "'";
				Component.SetExecutionState(Instance.InstanceID, components::BehaviorExecutionState::Failed, Instance.Diagnostic);
				throw BehaviorResolutionException(Instance.Diagnostic);
			}
			const auto Snapshot = std::ranges::find(State, Instance.InstanceID, &BehaviorStateSnapshot::InstanceID);
			const bool HasMigratableState =
				Snapshot != State.end() && Snapshot->Type == Instance.Type && Descriptor->RestoreState != nullptr;
			if (HasMigratableState)
			{
				Instance.Properties = Snapshot->Properties;
				Instance.SchemaVersion = Snapshot->SchemaVersion;
			}
			if (Descriptor->SchemaVersion != Instance.SchemaVersion && Descriptor->MigrateProperties == nullptr)
			{
				Instance.Diagnostic = "Behavior schema version mismatch for '" + Instance.TypeName + "'";
				Component.SetExecutionState(Instance.InstanceID, components::BehaviorExecutionState::Failed, Instance.Diagnostic);
				throw BehaviorResolutionException(Instance.Diagnostic);
			}
			if (Descriptor->SchemaVersion != Instance.SchemaVersion)
			{
				std::array<char, 2'048> Diagnostic{};
				if (!Descriptor->MigrateProperties(Instance.SchemaVersion, &Instance.Properties, Diagnostic.data(), Diagnostic.size()))
				{
					Instance.Diagnostic = "Behavior property migration failed for '" + Instance.TypeName + "': " +
										  (Diagnostic.front() == '\0' ? string("no diagnostic supplied") : string(Diagnostic.data()));
					Component.SetExecutionState(Instance.InstanceID, components::BehaviorExecutionState::Failed, Instance.Diagnostic);
					throw BehaviorResolutionException(Instance.Diagnostic);
				}
				Instance.SchemaVersion = Descriptor->SchemaVersion;
			}
			try
			{
				BehaviorRegistry::NormalizeProperties(*Descriptor, Instance.Properties);
			}
			catch (const BehaviorRegistryException &Exception)
			{
				Instance.Diagnostic = "Behavior property validation failed for '" + Instance.TypeName + "': " + Exception.what();
				Component.SetExecutionState(Instance.InstanceID, components::BehaviorExecutionState::Failed, Instance.Diagnostic);
				throw BehaviorResolutionException(Instance.Diagnostic);
			}
			Component.SetPropertiesAndSchema(Instance.InstanceID, Instance.SchemaVersion, Instance.Properties);

			this->Entries.push_back({.Owner = Component.GetOwner(),
									 .Component = ComponentHandle,
									 .InstanceID = Instance.InstanceID,
									 .Descriptor = *Descriptor,
									 .Properties = Instance.Properties});
		}
	}
	this->Statistics.RegisteredInstances = static_cast<uint32>(this->Entries.size());
}

void BehaviorRuntime::AllocateStorage()
{
	usize RequiredSize = 0;
	usize RequiredAlignment = alignof(std::max_align_t);
	for (RuntimeEntry &Entry : this->Entries)
	{
		RequiredAlignment = std::max(RequiredAlignment, Entry.Descriptor.StateAlignment);
		RequiredSize = AlignUp(RequiredSize, Entry.Descriptor.StateAlignment);
		Entry.StateOffset = RequiredSize;
		if (RequiredSize > std::numeric_limits<usize>::max() - Entry.Descriptor.StateSize)
			throw BehaviorLifecycleException("Behavior runtime state allocation exceeds addressable memory");
		RequiredSize += Entry.Descriptor.StateSize;
	}
	if (RequiredSize == 0)
		return;
	this->StorageAlignment = RequiredAlignment;
	this->StorageSize = AlignUp(RequiredSize, RequiredAlignment);
	this->Storage = ::operator new(this->StorageSize, std::align_val_t(this->StorageAlignment));
}

void BehaviorRuntime::ConstructAndStart(const std::span<const BehaviorStateSnapshot> State)
{
	for (RuntimeEntry &Entry : this->Entries)
	{
		try
		{
			world::SceneCommandBuffer LocalCommands;
			BehaviorExecutionContext Context(*this->Scene, LocalCommands, *this->Assets, Entry.Owner, Entry.InstanceID, Entry.Properties);
			Entry.Descriptor.Construct(this->GetState(Entry), &Context);
			Entry.Constructed = true;
			this->PendingCommands.Absorb(LocalCommands);
			this->SetInstanceState(Entry, components::BehaviorExecutionState::Constructed);
		}
		catch (...)
		{
			Entry.Failed = true;
			Entry.Failure = BehaviorRuntime::CurrentExceptionDiagnostic("Behavior construction failed");
			this->SetInstanceState(Entry, components::BehaviorExecutionState::Failed, Entry.Failure);
			throw BehaviorLifecycleException(Entry.Failure);
		}
	}

	for (RuntimeEntry &Entry : this->Entries)
	{
		const auto Snapshot = std::ranges::find(State, Entry.InstanceID, &BehaviorStateSnapshot::InstanceID);
		if (Snapshot == State.end() || !Snapshot->HasRuntimeState)
			continue;
		if (Snapshot->Type != Entry.Descriptor.Type || Entry.Descriptor.RestoreState == nullptr)
			throw BehaviorLifecycleException("Behavior '" + Entry.Descriptor.Name + "' cannot restore its captured reload state");
		std::array<char, 2'048> Diagnostic{};
		world::SceneCommandBuffer LocalCommands;
		BehaviorExecutionContext Context(*this->Scene, LocalCommands, *this->Assets, Entry.Owner, Entry.InstanceID, Entry.Properties);
		if (!Entry.Descriptor.RestoreState(this->GetState(Entry), Snapshot->Data.data(), Snapshot->Data.size(), Snapshot->SchemaVersion,
										   &Context, Diagnostic.data(), Diagnostic.size()))
		{
			throw BehaviorLifecycleException("Behavior '" + Entry.Descriptor.Name + "' state restoration failed: " +
											 (Diagnostic.front() == '\0' ? string("no diagnostic supplied") : string(Diagnostic.data())));
		}
		this->PendingCommands.Absorb(LocalCommands);
		{
			auto Access = this->Scene->Write();
			components::CObjectBehaviorComponent &Component = Access.Resolve(Entry.Component);
			const std::optional<components::BehaviorInstance> Instance = Component.FindBehavior(Entry.InstanceID);
			if (!Instance.has_value())
				throw BehaviorLifecycleException("Behavior instance disappeared during hot-reload restoration");
			Component.SetPropertiesAndSchema(Entry.InstanceID, Entry.Descriptor.SchemaVersion, Entry.Properties);
		}
	}

	for (RuntimeEntry &Entry : this->Entries)
	{
		try
		{
			if (Entry.Descriptor.Start != nullptr)
			{
				world::SceneCommandBuffer LocalCommands;
				BehaviorExecutionContext Context(*this->Scene, LocalCommands, *this->Assets, Entry.Owner, Entry.InstanceID,
												 Entry.Properties);
				Entry.Descriptor.Start(this->GetState(Entry), &Context);
				this->PendingCommands.Absorb(LocalCommands);
			}
			Entry.Started = true;
			this->SetInstanceState(Entry, components::BehaviorExecutionState::Active);
			++this->Statistics.ActiveInstances;
		}
		catch (...)
		{
			Entry.Failed = true;
			Entry.Failure = BehaviorRuntime::CurrentExceptionDiagnostic("Behavior start failed");
			this->SetInstanceState(Entry, components::BehaviorExecutionState::Failed, Entry.Failure);
			throw BehaviorLifecycleException(Entry.Failure);
		}
	}
}

void BehaviorRuntime::RunPhase(core::threading::TaskScheduler &Scheduler, const float32 DeltaSeconds, const CallbackPhase Phase)
{
	core::threading::TaskGroup ParallelGroup;
	try
	{
		for (RuntimeEntry &Entry : this->Entries)
		{
			if (!Entry.Started || Entry.Failed)
				continue;
			const BehaviorUpdateFunction Callback = Phase == CallbackPhase::Update ? Entry.Descriptor.Update : Entry.Descriptor.FixedUpdate;
			if (Callback == nullptr)
				continue;
			if (Entry.Descriptor.ParallelUpdateSafe)
			{
				ParallelGroup.Run(Scheduler, [this, &Entry, DeltaSeconds, Phase]() { this->InvokePhase(Entry, DeltaSeconds, Phase); });
				++this->Statistics.ParallelCallbacks;
			}
			else
			{
				this->InvokePhase(Entry, DeltaSeconds, Phase);
				++this->Statistics.SerialCallbacks;
			}
		}
	}
	catch (...)
	{
		ParallelGroup.Wait();
		for (RuntimeEntry &Entry : this->Entries)
			Entry.StagedCommands.reset();
		throw;
	}
	ParallelGroup.Wait();
	for (RuntimeEntry &Entry : this->Entries)
	{
		if (Entry.StagedCommands == nullptr)
			continue;
		this->PendingCommands.Absorb(*Entry.StagedCommands);
		Entry.StagedCommands.reset();
	}
	this->RetireFailedEntries();
	this->ExecutePendingCommands();
}

void BehaviorRuntime::InvokePhase(RuntimeEntry &Entry, const float32 DeltaSeconds, const CallbackPhase Phase)
{
	try
	{
		auto LocalCommands = std::make_unique<world::SceneCommandBuffer>();
		BehaviorExecutionContext Context(*this->Scene, *LocalCommands, *this->Assets, Entry.Owner, Entry.InstanceID, Entry.Properties);
		const BehaviorUpdateFunction Callback = Phase == CallbackPhase::Update ? Entry.Descriptor.Update : Entry.Descriptor.FixedUpdate;
		Callback(this->GetState(Entry), &Context, DeltaSeconds);
		Entry.StagedCommands = std::move(LocalCommands);
	}
	catch (...)
	{
		Entry.Failed = true;
		Entry.Failure = BehaviorRuntime::CurrentExceptionDiagnostic(Phase == CallbackPhase::Update ? "Behavior update failed"
																								   : "Behavior fixed update failed");
	}
}

void BehaviorRuntime::RetireFailedEntries()
{
	for (RuntimeEntry &Entry : this->Entries)
	{
		if (!Entry.Failed || !Entry.Started)
			continue;
		if (Entry.Descriptor.Stop != nullptr)
		{
			try
			{
				world::SceneCommandBuffer LocalCommands;
				BehaviorExecutionContext Context(*this->Scene, LocalCommands, *this->Assets, Entry.Owner, Entry.InstanceID,
												 Entry.Properties);
				Entry.Descriptor.Stop(this->GetState(Entry), &Context);
				this->PendingCommands.Absorb(LocalCommands);
			}
			catch (...)
			{
				Entry.Failure += "; " + BehaviorRuntime::CurrentExceptionDiagnostic("cleanup stop failed");
			}
		}
		Entry.Started = false;
		if (Entry.Constructed)
		{
			Entry.Descriptor.Destroy(this->GetState(Entry));
			Entry.Constructed = false;
		}
		if (this->Statistics.ActiveInstances != 0)
			--this->Statistics.ActiveInstances;
		++this->Statistics.FailedInstances;
		this->SetInstanceState(Entry, components::BehaviorExecutionState::Failed, Entry.Failure);
	}
}

void BehaviorRuntime::ExecutePendingCommands()
{
	if (!this->PendingCommands.Empty())
		this->PendingCommands.Execute(*this->Scene);
}

void BehaviorRuntime::ReleaseEntries(const bool PreserveFailures) noexcept
{
	for (usize Index = this->Entries.size(); Index-- > 0;)
	{
		RuntimeEntry &Entry = this->Entries[Index];
		if (Entry.Started && Entry.Descriptor.Stop != nullptr)
		{
			try
			{
				world::SceneCommandBuffer DiscardedCommands;
				BehaviorExecutionContext Context(*this->Scene, DiscardedCommands, *this->Assets, Entry.Owner, Entry.InstanceID,
												 Entry.Properties);
				Entry.Descriptor.Stop(this->GetState(Entry), &Context);
			}
			catch (...)
			{
			}
		}
		if (Entry.Constructed)
			Entry.Descriptor.Destroy(this->GetState(Entry));
		if (!PreserveFailures || !Entry.Failed)
			this->SetInstanceState(Entry, components::BehaviorExecutionState::Unresolved);
	}
	if (this->Storage != nullptr)
		::operator delete(this->Storage, std::align_val_t(this->StorageAlignment));
	this->Storage = nullptr;
	this->StorageSize = 0;
	this->StorageAlignment = alignof(std::max_align_t);
	this->Entries.clear();
	this->PendingCommands.Clear();
	this->Running = false;
	this->Statistics.ActiveInstances = 0;
}

void BehaviorRuntime::SetInstanceState(const RuntimeEntry &Entry, const components::BehaviorExecutionState State,
									   string Diagnostic) noexcept
{
	try
	{
		auto Access = this->Scene->Write();
		components::CObjectBehaviorComponent &Component = Access.Resolve(Entry.Component);
		Component.SetExecutionState(Entry.InstanceID, State, std::move(Diagnostic));
	}
	catch (...)
	{
	}
}

void *BehaviorRuntime::GetState(const RuntimeEntry &Entry) const noexcept
{
	return static_cast<std::byte *>(this->Storage) + Entry.StateOffset;
}

const void *BehaviorRuntime::GetStateConst(const RuntimeEntry &Entry) const noexcept
{
	return static_cast<const std::byte *>(this->Storage) + Entry.StateOffset;
}

string BehaviorRuntime::CurrentExceptionDiagnostic(const string_view Prefix) noexcept
{
	try
	{
		try
		{
			throw;
		}
		catch (const std::exception &Exception)
		{
			return string(Prefix) + ": " + Exception.what();
		}
		catch (...)
		{
			return string(Prefix) + ": non-standard exception";
		}
	}
	catch (...)
	{
		return string(Prefix);
	}
}
} // namespace runtime::behavior
