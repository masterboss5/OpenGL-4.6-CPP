#pragma once

#include "SelectionSet.h"
#include "Source/editor/commands/CommandHistory.h"
#include "Source/editor/instance/InstanceGraph.h"
#include "Source/scene/Scene.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <filesystem>
#include <memory>
#include <thread>

namespace resource
{
class AssetManager;
}
namespace runtime::behavior
{
class BehaviorRegistry;
}
namespace editor::asset
{
class PrimitiveMeshFactory;
}

namespace editor::document
{
struct SceneObjectSpecification final
{
	string Name = "Object";
	world::ObjectHandle Parent;
	util::UUID PersistentID = util::UUID::GenerateRandomUUID();
};

class SceneDocument final
{
  public:
	explicit SceneDocument(string Name = "Untitled", world::SceneCapacitySpecification Capacity = {},
						   util::UUID ID = util::UUID::GenerateRandomUUID(), usize CommandHistoryCapacity = 4'096);

	SceneDocument(const SceneDocument &) = delete;
	SceneDocument &operator=(const SceneDocument &) = delete;
	SceneDocument(SceneDocument &&) = delete;
	SceneDocument &operator=(SceneDocument &&) = delete;

	[[nodiscard]] world::ObjectHandle CreateObject(string Name, world::ObjectHandle Parent = {},
												   util::UUID PersistentID = util::UUID::GenerateRandomUUID());
	[[nodiscard]] world::ObjectHandle CreateObject(SceneObjectSpecification Specification);
	[[nodiscard]] util::UUID CreateInstance(instance::InstanceClassID ClassID, util::UUID Parent, string Name = {},
											util::UUID ID = util::UUID::GenerateRandomUUID());
	void DestroyInstance(const util::UUID &ID);
	void RenameInstance(const util::UUID &ID, string Name);
	void ReparentInstance(const util::UUID &ID, const util::UUID &Parent, uint32 SiblingOrder = 0);
	void SetInstanceProperty(const util::UUID &ID, string Name, instance::InstancePropertyValue Value);
	void RemoveInstanceProperty(const util::UUID &ID, string_view Name);
	void SetInstanceWorldTransform(const util::UUID &ID, const glm::vec3 &Position, const glm::quat &Rotation, const glm::vec3 &Scale);
	void ConfigureRuntimeAssets(resource::AssetManager &Assets, asset::PrimitiveMeshFactory &Primitives,
								runtime::behavior::BehaviorRegistry *Behaviors = nullptr);
	void SynchronizeAllRuntimeBackings();
	void DestroyObject(const util::UUID &PersistentID);
	void SetParent(const util::UUID &Object, const util::UUID &Parent, uint32 SiblingOrder = 0);
	void Execute(commands::EditorCommandPtr Command);
	void Undo();
	void Redo();
	void MarkSaved(std::filesystem::path Path);
	void MarkRecovered(std::filesystem::path OriginalPath);
	void MarkModified() noexcept;

	[[nodiscard]] world::Scene &GetScene() noexcept;
	[[nodiscard]] const world::Scene &GetScene() const noexcept;
	[[nodiscard]] instance::InstanceGraph &GetInstances() noexcept;
	[[nodiscard]] const instance::InstanceGraph &GetInstances() const noexcept;
	[[nodiscard]] instance::InstanceTypeRegistry &GetInstanceTypes() noexcept;
	[[nodiscard]] const instance::InstanceTypeRegistry &GetInstanceTypes() const noexcept;
	[[nodiscard]] SelectionSet &GetSelection() noexcept;
	[[nodiscard]] const SelectionSet &GetSelection() const noexcept;
	[[nodiscard]] commands::CommandHistory &GetHistory() noexcept;
	[[nodiscard]] const util::UUID &GetID() const noexcept;
	[[nodiscard]] const string &GetName() const noexcept;
	void SetName(string Name);
	[[nodiscard]] const std::filesystem::path &GetPath() const noexcept;
	[[nodiscard]] uint64 GetRevision() const noexcept;
	[[nodiscard]] bool IsDirty() const noexcept;
	[[nodiscard]] const string &GetPreservedSerializationData() const noexcept;
	void SetPreservedSerializationData(string Data);
	void AssertOwnerThread() const;

  private:
	util::UUID ID = util::UUID::GenerateRandomUUID();
	string Name;
	std::filesystem::path Path;
	instance::InstanceTypeRegistry InstanceTypes;
	instance::InstanceGraph Instances;
	std::unique_ptr<world::Scene> Scene;
	SelectionSet Selection;
	commands::CommandHistory History;
	uint64 Revision = 1;
	uint64 SavedRevision = 0;
	uint64 RevisionSceneBaseline = 1;
	string PreservedSerializationData;
	std::thread::id OwnerThread;
	resource::AssetManager *Assets = nullptr;
	asset::PrimitiveMeshFactory *Primitives = nullptr;
	runtime::behavior::BehaviorRegistry *Behaviors = nullptr;

	[[nodiscard]] world::ObjectHandle CreateRuntimeObject(string Name, const util::UUID &PersistentID);
	void CreateRuntimeBacking(const util::UUID &ID);
	void SynchronizeRuntimeBacking(const util::UUID &ID);
	void SynchronizeRuntimeSubtree(const util::UUID &ID);
	void SynchronizeRuntimeBehaviors(const util::UUID &ParentID);
	void SynchronizeRuntimeAnimations(const util::UUID &ModelID);
	[[nodiscard]] util::UUID FindAnimationModel(const util::UUID &ID) const;
};
} // namespace editor::document

namespace editor
{
// SceneDocument is the authoritative editor document implementation.
using EditorDocument = document::SceneDocument;
} // namespace editor
