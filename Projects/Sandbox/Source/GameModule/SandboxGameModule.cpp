#include "src/runtime/module/GameModuleAPI.h"

#include <array>
#include <cstring>
#include <new>

namespace
{
struct SandboxBehaviorState final
{
	float32 ElapsedSeconds = 0.0f;
};

const std::array SandboxBehaviorProperties{runtime::behavior::BehaviorPropertyDescriptor{.Name = "Speed", .DefaultValue = float32{1.0f}}};

void Construct(void *State, runtime::behavior::BehaviorExecutionContext *)
{
	::new (State) SandboxBehaviorState();
}

void Start(void *, runtime::behavior::BehaviorExecutionContext *)
{
}

void Update(void *State, runtime::behavior::BehaviorExecutionContext *, const float32 DeltaSeconds)
{
	static_cast<SandboxBehaviorState *>(State)->ElapsedSeconds += DeltaSeconds;
}

void FixedUpdate(void *, runtime::behavior::BehaviorExecutionContext *, float32)
{
}

void Stop(void *, runtime::behavior::BehaviorExecutionContext *)
{
}

void Destroy(void *State) noexcept
{
	static_cast<SandboxBehaviorState *>(State)->~SandboxBehaviorState();
}

bool SerializeState(const void *State, std::byte *Destination, const usize Capacity, usize *WrittenSize, char *, usize) noexcept
{
	if (State == nullptr || WrittenSize == nullptr)
		return false;
	*WrittenSize = sizeof(SandboxBehaviorState);
	if (Destination == nullptr)
		return Capacity == 0;
	if (Capacity < sizeof(SandboxBehaviorState))
		return false;
	std::memcpy(Destination, State, sizeof(SandboxBehaviorState));
	return true;
}

bool RestoreState(void *State, const std::byte *Source, const usize Size, const uint32 SourceSchemaVersion,
				  runtime::behavior::BehaviorExecutionContext *, char *, usize) noexcept
{
	if (State == nullptr || Source == nullptr || SourceSchemaVersion != 1 || Size != sizeof(SandboxBehaviorState))
		return false;
	std::memcpy(State, Source, sizeof(SandboxBehaviorState));
	return true;
}

bool Register(const runtime::module::GameModuleRegistrar *Registrar, char *Diagnostic, const usize DiagnosticCapacity) noexcept
{
	if (Registrar == nullptr || Registrar->ABIVersion != runtime::module::GameModuleABIVersion ||
		Registrar->StructureSize < sizeof(runtime::module::GameModuleRegistrar) ||
		Registrar->CompilerVersion != runtime::module::GameModuleCompilerVersion ||
		Registrar->BuildFlags != runtime::module::GameModuleBuildFlags || Registrar->RegisterBehavior == nullptr)
		return false;
	const runtime::module::GameModuleBehaviorDescriptor Descriptor{.Type = 1,
																   .Name = "SandboxBehavior",
																   .SchemaVersion = 1,
																   .StateSize = sizeof(SandboxBehaviorState),
																   .StateAlignment = alignof(SandboxBehaviorState),
																   .ParallelUpdateSafe = 1,
																   .Properties = SandboxBehaviorProperties.data(),
																   .PropertyCount = SandboxBehaviorProperties.size(),
																   .Construct = &Construct,
																   .Start = &Start,
																   .Update = &Update,
																   .FixedUpdate = &FixedUpdate,
																   .Stop = &Stop,
																   .Destroy = &Destroy,
																   .SerializeState = &SerializeState,
																   .RestoreState = &RestoreState};
	return Registrar->RegisterBehavior(Registrar->UserData, &Descriptor, Diagnostic, DiagnosticCapacity);
}

void OnLoad() noexcept
{
}

bool PrepareReload(char *, usize) noexcept
{
	return true;
}

void OnUnload() noexcept
{
}

const runtime::module::GameModuleExports Exports{.ABIVersion = runtime::module::GameModuleABIVersion,
												 .StructureSize = sizeof(runtime::module::GameModuleExports),
												 .Name = "Sandbox",
												 .Register = &Register,
												 .PrepareReload = &PrepareReload,
												 .OnLoad = &OnLoad,
												 .OnUnload = &OnUnload};
} // namespace

OPENGL_GAME_MODULE_ENTRY_POINT const runtime::module::GameModuleExports *GetGameModuleExports() noexcept
{
	return &Exports;
}
