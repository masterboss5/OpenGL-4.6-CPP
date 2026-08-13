#pragma once

#include "Source/runtime/behavior/BehaviorRegistry.h"
#include "Source/types.h"

#include <cstddef>
#include <type_traits>

namespace runtime::module
{
inline constexpr uint32 GameModuleABIVersion = 3;
inline constexpr string_view GameModuleExportName = "GetGameModuleExports";
inline constexpr uint32 GameModuleCompilerVersion = _MSC_VER;
inline constexpr uint32 GameModuleBuildFlagDebug = 1U << 0U;
inline constexpr uint32 GameModuleBuildFlagDynamicRuntime = 1U << 1U;
inline constexpr uint32 GameModuleBuildFlags =
#ifdef _DEBUG
	GameModuleBuildFlagDebug |
#endif
#ifdef _DLL
	GameModuleBuildFlagDynamicRuntime;
#else
	0U;
#endif

#define OPENGL_GAME_MODULE_ENTRY_POINT extern "C" __declspec(dllexport)

static_assert(sizeof(void *) == 8, "The game-module ABI requires the engine's x64 target");
static_assert(sizeof(usize) == sizeof(uint64), "The game-module ABI requires 64-bit engine size aliases");

struct GameModuleBehaviorDescriptor final
{
	components::BehaviorTypeID Type = 0;
	const char *Name = nullptr;
	uint32 SchemaVersion = 0;
	uint64 StateSize = 0;
	uint64 StateAlignment = 0;
	uint8 ParallelUpdateSafe = 0;
	const behavior::BehaviorPropertyDescriptor *Properties = nullptr;
	uint64 PropertyCount = 0;
	behavior::BehaviorConstructFunction Construct = nullptr;
	behavior::BehaviorLifecycleFunction Start = nullptr;
	behavior::BehaviorUpdateFunction Update = nullptr;
	behavior::BehaviorUpdateFunction FixedUpdate = nullptr;
	behavior::BehaviorLifecycleFunction Stop = nullptr;
	behavior::BehaviorDestroyFunction Destroy = nullptr;
	behavior::BehaviorSerializeStateFunction SerializeState = nullptr;
	behavior::BehaviorRestoreStateFunction RestoreState = nullptr;
	behavior::BehaviorMigratePropertiesFunction MigrateProperties = nullptr;
};

using RegisterGameBehaviorFunction = bool (*)(void *UserData, const GameModuleBehaviorDescriptor *Descriptor, char *Diagnostic,
											  uint64 DiagnosticCapacity) noexcept;

struct GameModuleRegistrar final
{
	uint32 ABIVersion = GameModuleABIVersion;
	uint32 StructureSize = sizeof(GameModuleRegistrar);
	uint32 CompilerVersion = GameModuleCompilerVersion;
	uint32 BuildFlags = GameModuleBuildFlags;
	void *UserData = nullptr;
	RegisterGameBehaviorFunction RegisterBehavior = nullptr;
};

using RegisterGameModuleFunction = bool (*)(const GameModuleRegistrar *Registrar, char *Diagnostic, uint64 DiagnosticCapacity) noexcept;
using GameModuleLifecycleFunction = void (*)() noexcept;
using GameModulePrepareReloadFunction = bool (*)(char *Diagnostic, uint64 DiagnosticCapacity) noexcept;

struct GameModuleExports final
{
	uint32 ABIVersion = GameModuleABIVersion;
	uint32 StructureSize = sizeof(GameModuleExports);
	uint32 CompilerVersion = GameModuleCompilerVersion;
	uint32 BuildFlags = GameModuleBuildFlags;
	const char *Name = nullptr;
	RegisterGameModuleFunction Register = nullptr;
	GameModulePrepareReloadFunction PrepareReload = nullptr;
	GameModuleLifecycleFunction OnLoad = nullptr;
	GameModuleLifecycleFunction OnUnload = nullptr;
};

using GetGameModuleExportsFunction = const GameModuleExports *(*)() noexcept;

static_assert(std::is_standard_layout_v<GameModuleBehaviorDescriptor> && std::is_trivially_copyable_v<GameModuleBehaviorDescriptor>);
static_assert(std::is_standard_layout_v<GameModuleRegistrar> && std::is_trivially_copyable_v<GameModuleRegistrar>);
static_assert(std::is_standard_layout_v<GameModuleExports> && std::is_trivially_copyable_v<GameModuleExports>);
static_assert(sizeof(GameModuleRegistrar) == 32 && alignof(GameModuleRegistrar) == 8);
static_assert(offsetof(GameModuleRegistrar, UserData) == 16 && offsetof(GameModuleRegistrar, RegisterBehavior) == 24);
static_assert(sizeof(GameModuleExports) == 56 && alignof(GameModuleExports) == 8);
static_assert(offsetof(GameModuleExports, Name) == 16 && offsetof(GameModuleExports, Register) == 24 &&
			  offsetof(GameModuleExports, OnUnload) == 48);
static_assert(sizeof(GameModuleBehaviorDescriptor) == 136 && alignof(GameModuleBehaviorDescriptor) == 8);
static_assert(offsetof(GameModuleBehaviorDescriptor, Name) == 8 && offsetof(GameModuleBehaviorDescriptor, StateSize) == 24 &&
			  offsetof(GameModuleBehaviorDescriptor, Properties) == 48 && offsetof(GameModuleBehaviorDescriptor, Construct) == 64 &&
			  offsetof(GameModuleBehaviorDescriptor, MigrateProperties) == 128);
} // namespace runtime::module
