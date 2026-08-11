#pragma once

#include "SelectionSet.h"
#include "src/editor/commands/CommandHistory.h"
#include "src/scene/Scene.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <filesystem>
#include <memory>
#include <thread>

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
	std::unique_ptr<world::Scene> Scene;
	SelectionSet Selection;
	commands::CommandHistory History;
	uint64 Revision = 1;
	uint64 SavedRevision = 0;
	uint64 RevisionSceneBaseline = 1;
	string PreservedSerializationData;
	std::thread::id OwnerThread;
};
} // namespace editor::document

namespace editor
{
// SceneDocument is the authoritative editor document implementation.
using EditorDocument = document::SceneDocument;
} // namespace editor
