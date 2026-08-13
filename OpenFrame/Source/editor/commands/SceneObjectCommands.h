#pragma once

#include "EditorCommand.h"
#include "Source/resource/asset/ModelAsset.h"
#include "Source/util/UUID.h"

#include <memory>
#include <functional>
#include <span>
#include <vector>

#include <glm.hpp>
#include <gtc/quaternion.hpp>

namespace editor::document
{
class SceneDocument;
}

namespace editor::commands
{
enum class ReparentTransformRule : uint8
{
	PreserveWorld,
	PreserveLocal
};

class SceneObjectSnapshot final
{
  public:
	[[nodiscard]] static SceneObjectSnapshot Capture(document::SceneDocument &Document, std::span<const util::UUID> Objects);

	SceneObjectSnapshot(const SceneObjectSnapshot &Other);
	SceneObjectSnapshot &operator=(const SceneObjectSnapshot &Other);
	SceneObjectSnapshot(SceneObjectSnapshot &&Other) noexcept;
	SceneObjectSnapshot &operator=(SceneObjectSnapshot &&Other) noexcept;
	~SceneObjectSnapshot();

	[[nodiscard]] bool Empty() const noexcept;
	[[nodiscard]] usize GetObjectCount() const noexcept;

  private:
	class Storage;
	std::unique_ptr<Storage> State;

	explicit SceneObjectSnapshot(std::unique_ptr<Storage> State) noexcept;
	friend class PasteObjectsCommand;
};

class CreateObjectCommand final : public EditorCommand
{
  public:
	CreateObjectCommand(document::SceneDocument &Document, string Name, util::UUID Parent = {});

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] const util::UUID &GetPersistentID() const noexcept;

  private:
	document::SceneDocument *Document = nullptr;
	string Name;
	util::UUID PersistentID = util::UUID::GenerateRandomUUID();
	util::UUID Parent;
	std::vector<util::UUID> PreviousSelection;
	bool Present = false;
};

class CreateMeshObjectCommand final : public EditorCommand
{
  public:
	CreateMeshObjectCommand(document::SceneDocument &Document, string Name, resource::AssetHandle<resource::ModelAsset> Model,
							util::UUID Parent = {});

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] const util::UUID &GetPersistentID() const noexcept;

  private:
	document::SceneDocument *Document = nullptr;
	string Name;
	resource::AssetHandle<resource::ModelAsset> Model;
	util::UUID PersistentID = util::UUID::GenerateRandomUUID();
	util::UUID Parent;
	std::vector<util::UUID> PreviousSelection;
	bool Present = false;
};

class RenameObjectCommand final : public EditorCommand
{
  public:
	RenameObjectCommand(document::SceneDocument &Document, util::UUID Object, string Name);

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] bool TryMerge(const EditorCommand &Other) override;

  private:
	void Apply(string_view Value);

	document::SceneDocument *Document = nullptr;
	util::UUID Object;
	string Before;
	string After;
};

class ReparentObjectCommand final : public EditorCommand
{
  public:
	ReparentObjectCommand(document::SceneDocument &Document, util::UUID Object, util::UUID Parent, uint32 SiblingOrder = 0,
						  ReparentTransformRule TransformRule = ReparentTransformRule::PreserveWorld);

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	struct SiblingOrderEdit final
	{
		util::UUID Object;
		util::UUID Parent;
		uint32 Before = 0;
		uint32 After = 0;
	};

	document::SceneDocument *Document = nullptr;
	util::UUID Object;
	util::UUID BeforeParent;
	util::UUID AfterParent;
	uint32 BeforeSiblingOrder = 0;
	uint32 AfterSiblingOrder = 0;
	glm::vec3 BeforePosition{0.0f};
	glm::quat BeforeRotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 BeforeScale{1.0f};
	glm::vec3 AfterPosition{0.0f};
	glm::quat AfterRotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 AfterScale{1.0f};
	std::vector<SiblingOrderEdit> SiblingOrderEdits;
};

class AddComponentCommand final : public EditorCommand
{
  public:
	AddComponentCommand(document::SceneDocument &Document, util::UUID Object, uint32 ComponentType);

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	document::SceneDocument *Document = nullptr;
	util::UUID Object;
	uint32 ComponentType = 0;
	bool Present = false;
};

class RemoveComponentCommand final : public EditorCommand
{
  public:
	RemoveComponentCommand(document::SceneDocument &Document, util::UUID Object, uint32 ComponentType);
	~RemoveComponentCommand() override;

	RemoveComponentCommand(const RemoveComponentCommand &) = delete;
	RemoveComponentCommand &operator=(const RemoveComponentCommand &) = delete;
	RemoveComponentCommand(RemoveComponentCommand &&) = delete;
	RemoveComponentCommand &operator=(RemoveComponentCommand &&) = delete;

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	class State;

	document::SceneDocument *Document = nullptr;
	util::UUID Object;
	uint32 ComponentType = 0;
	std::unique_ptr<State> Before;
	bool Present = true;
};

class DeleteObjectsCommand final : public EditorCommand
{
  public:
	using FinalizationCallback = std::function<void(std::vector<resource::AssetID>)>;

	DeleteObjectsCommand(document::SceneDocument &Document, std::vector<util::UUID> Objects,
						 FinalizationCallback FinalizeDeletedAssets = {});
	~DeleteObjectsCommand() override;

	DeleteObjectsCommand(const DeleteObjectsCommand &) = delete;
	DeleteObjectsCommand &operator=(const DeleteObjectsCommand &) = delete;
	DeleteObjectsCommand(DeleteObjectsCommand &&) = delete;
	DeleteObjectsCommand &operator=(DeleteObjectsCommand &&) = delete;

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	void Finalize() override;

  private:
	class Archive;

	document::SceneDocument *Document = nullptr;
	std::unique_ptr<Archive> State;
	std::vector<util::UUID> PreviousSelection;
	FinalizationCallback FinalizeDeletedAssets;
	bool Present = true;
	bool Finalized = false;
};

class DuplicateObjectsCommand final : public EditorCommand
{
  public:
	DuplicateObjectsCommand(document::SceneDocument &Document, std::vector<util::UUID> Objects);
	~DuplicateObjectsCommand() override;

	DuplicateObjectsCommand(const DuplicateObjectsCommand &) = delete;
	DuplicateObjectsCommand &operator=(const DuplicateObjectsCommand &) = delete;
	DuplicateObjectsCommand(DuplicateObjectsCommand &&) = delete;
	DuplicateObjectsCommand &operator=(DuplicateObjectsCommand &&) = delete;

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] const std::vector<util::UUID> &GetCreatedObjects() const noexcept;

  private:
	class Archive;

	document::SceneDocument *Document = nullptr;
	std::unique_ptr<Archive> State;
	std::vector<util::UUID> PreviousSelection;
	bool Present = false;
};

class PasteObjectsCommand final : public EditorCommand
{
  public:
	PasteObjectsCommand(document::SceneDocument &Document, const SceneObjectSnapshot &Snapshot);
	~PasteObjectsCommand() override;

	PasteObjectsCommand(const PasteObjectsCommand &) = delete;
	PasteObjectsCommand &operator=(const PasteObjectsCommand &) = delete;
	PasteObjectsCommand(PasteObjectsCommand &&) = delete;
	PasteObjectsCommand &operator=(PasteObjectsCommand &&) = delete;

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] const std::vector<util::UUID> &GetCreatedObjects() const noexcept;

  private:
	class Archive;

	document::SceneDocument *Document = nullptr;
	std::unique_ptr<Archive> State;
	std::vector<util::UUID> PreviousSelection;
	bool Present = false;
};
} // namespace editor::commands
