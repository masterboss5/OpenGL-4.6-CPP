#pragma once

#include "Source/component/object/CObjectBehaviorComponent.h"
#include "Source/types.h"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace resource
{
class AssetManager;
}

namespace world
{
class Scene;
class SceneCommandBuffer;
} // namespace world

namespace runtime::behavior
{
class ENGINE_API BehaviorExecutionContext final
{
  public:
	BehaviorExecutionContext(world::Scene &Scene, world::SceneCommandBuffer &Commands, resource::AssetManager &Assets,
							 world::ObjectHandle Owner, util::UUID InstanceID,
							 const std::unordered_map<string, components::BehaviorPropertyValue> &Properties) noexcept;

	[[nodiscard]] world::Scene &GetScene() const noexcept;
	[[nodiscard]] world::SceneCommandBuffer &GetCommands() const noexcept;
	[[nodiscard]] resource::AssetManager &GetAssets() const noexcept;
	[[nodiscard]] world::ObjectHandle GetOwner() const noexcept;
	[[nodiscard]] const util::UUID &GetInstanceID() const noexcept;
	[[nodiscard]] const components::BehaviorPropertyValue *FindProperty(string_view Name) const;

  private:
	world::Scene *Scene = nullptr;
	world::SceneCommandBuffer *Commands = nullptr;
	resource::AssetManager *Assets = nullptr;
	world::ObjectHandle Owner;
	util::UUID InstanceID;
	const std::unordered_map<string, components::BehaviorPropertyValue> *Properties = nullptr;
};

using BehaviorConstructFunction = void (*)(void *State, BehaviorExecutionContext *Context);
using BehaviorLifecycleFunction = void (*)(void *State, BehaviorExecutionContext *Context);
using BehaviorUpdateFunction = void (*)(void *State, BehaviorExecutionContext *Context, float32 DeltaSeconds);
using BehaviorDestroyFunction = void (*)(void *State) noexcept;
using BehaviorSerializeStateFunction = bool (*)(const void *State, std::byte *Destination, usize Capacity, usize *WrittenSize,
												char *Diagnostic, usize DiagnosticCapacity) noexcept;
using BehaviorRestoreStateFunction = bool (*)(void *State, const std::byte *Source, usize Size, uint32 SourceSchemaVersion,
											  BehaviorExecutionContext *Context, char *Diagnostic, usize DiagnosticCapacity) noexcept;
using BehaviorMigratePropertiesFunction = bool (*)(uint32 SourceSchemaVersion,
												   std::unordered_map<string, components::BehaviorPropertyValue> *Properties,
												   char *Diagnostic, usize DiagnosticCapacity) noexcept;

struct BehaviorPropertyDescriptor final
{
	string Name;
	components::BehaviorPropertyValue DefaultValue;
	bool ReadOnly = false;
	std::optional<float64> NumericMinimum;
	std::optional<float64> NumericMaximum;
	usize MaximumStringBytes = 64U * 1024U;
};

struct BehaviorDescriptor final
{
	components::BehaviorTypeID Type = 0;
	string Name;
	string ModuleName = "Engine";
	util::UUID StableTypeID;
	uint32 SchemaVersion = 0;
	usize StateSize = 0;
	usize StateAlignment = 0;
	bool ParallelUpdateSafe = false;
	std::vector<BehaviorPropertyDescriptor> Properties;
	BehaviorConstructFunction Construct = nullptr;
	BehaviorLifecycleFunction Start = nullptr;
	BehaviorUpdateFunction Update = nullptr;
	BehaviorUpdateFunction FixedUpdate = nullptr;
	BehaviorLifecycleFunction Stop = nullptr;
	BehaviorDestroyFunction Destroy = nullptr;
	BehaviorSerializeStateFunction SerializeState = nullptr;
	BehaviorRestoreStateFunction RestoreState = nullptr;
	BehaviorMigratePropertiesFunction MigrateProperties = nullptr;
	// Keeps the code module that owns this descriptor's callbacks loaded for as
	// long as any registry snapshot or runtime entry may invoke them.
	std::shared_ptr<const void> ModuleLease;
};

class ENGINE_API BehaviorRegistryException : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API InvalidBehaviorDescriptorException final : public BehaviorRegistryException
{
  public:
	using BehaviorRegistryException::BehaviorRegistryException;
};

class ENGINE_API DuplicateBehaviorDescriptorException final : public BehaviorRegistryException
{
  public:
	using BehaviorRegistryException::BehaviorRegistryException;
};

class ENGINE_API BehaviorRegistry final
{
  public:
	void Register(BehaviorDescriptor Descriptor);
	void ReplaceAll(std::span<const BehaviorDescriptor> Descriptors);
	void RestoreSnapshot(std::span<const BehaviorDescriptor> Descriptors);
	void Clear();

	[[nodiscard]] bool Contains(components::BehaviorTypeID Type) const;
	[[nodiscard]] std::optional<BehaviorDescriptor> Find(components::BehaviorTypeID Type) const;
	void SnapshotInto(std::vector<BehaviorDescriptor> &Result) const;
	[[nodiscard]] std::vector<BehaviorDescriptor> Snapshot() const;
	[[nodiscard]] uint64 GetGeneration() const;
	static void NormalizeProperties(const BehaviorDescriptor &Descriptor,
									std::unordered_map<string, components::BehaviorPropertyValue> &Properties);

  private:
	[[nodiscard]] static BehaviorDescriptor Validate(BehaviorDescriptor Descriptor);
	static void ValidateReplacementCompatibility(const BehaviorDescriptor &Previous, const BehaviorDescriptor &Replacement);

	mutable std::shared_mutex Mutex;
	std::unordered_map<components::BehaviorTypeID, BehaviorDescriptor> Descriptors;
	std::unordered_map<string, components::BehaviorTypeID> TypesByName;
	uint64 Generation = 1;
};

// GameBehaviorRegistry is the plan-facing name for the shared project behavior registry.
using GameBehaviorRegistry = BehaviorRegistry;
} // namespace runtime::behavior
