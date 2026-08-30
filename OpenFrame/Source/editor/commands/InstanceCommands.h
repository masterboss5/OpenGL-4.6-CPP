#pragma once

#include "Source/editor/commands/EditorCommand.h"
#include "Source/editor/instance/InstanceTypes.h"

#include <span>
#include <unordered_map>

namespace editor::instance
{
class InstanceGraph;
}

namespace editor::document
{
class SceneDocument;
}

namespace editor::commands
{
struct InstanceArchive final
{
	std::vector<instance::InstanceRecord> Records;
	std::vector<util::UUID> Roots;

	[[nodiscard]] bool Empty() const noexcept
	{
		return this->Roots.empty();
	}
};

[[nodiscard]] InstanceArchive CaptureInstanceArchive(const instance::InstanceGraph &Graph, std::span<const util::UUID> Selection);

class CreateInstanceCommand final : public EditorCommand
{
  public:
	CreateInstanceCommand(document::SceneDocument &Document, instance::InstanceClassID ClassID, util::UUID Parent,
						  instance::InstancePropertyMap InitialProperties = {});

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] const util::UUID &GetInstanceID() const noexcept;

  private:
	document::SceneDocument *Document = nullptr;
	instance::InstanceClassID ClassID;
	util::UUID Parent;
	instance::InstancePropertyMap InitialProperties;
	util::UUID ID = util::UUID::GenerateRandomUUID();
	std::vector<util::UUID> PreviousSelection;
	bool Present = false;
};

class RenameInstanceCommand final : public EditorCommand
{
  public:
	RenameInstanceCommand(document::SceneDocument &Document, util::UUID ID, string Name);
	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	document::SceneDocument *Document = nullptr;
	util::UUID ID;
	string Before;
	string After;
	bool Applied = false;
};

class ReparentInstanceCommand final : public EditorCommand
{
  public:
	ReparentInstanceCommand(document::SceneDocument &Document, util::UUID ID, util::UUID Parent, uint32 SiblingOrder);
	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	document::SceneDocument *Document = nullptr;
	util::UUID ID;
	util::UUID BeforeParent;
	util::UUID AfterParent;
	uint32 BeforeOrder = 0;
	uint32 AfterOrder = 0;
	bool Applied = false;
};

class SetInstancePropertyCommand final : public EditorCommand
{
  public:
	SetInstancePropertyCommand(document::SceneDocument &Document, util::UUID ID, string Name, instance::InstancePropertyValue Value);
	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] bool TryMerge(const EditorCommand &Other) override;

  private:
	document::SceneDocument *Document = nullptr;
	util::UUID ID;
	string Name;
	std::optional<instance::InstancePropertyValue> Before;
	instance::InstancePropertyValue After;
	bool Applied = false;
};

class RemoveInstancePropertyCommand final : public EditorCommand
{
  public:
	RemoveInstancePropertyCommand(document::SceneDocument &Document, util::UUID ID, string Name);
	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	document::SceneDocument *Document = nullptr;
	util::UUID ID;
	string Name;
	instance::InstancePropertyValue Before;
	bool Removed = false;
};

class DeleteInstanceCommand final : public EditorCommand
{
  public:
	DeleteInstanceCommand(document::SceneDocument &Document, util::UUID ID);
	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	document::SceneDocument *Document = nullptr;
	util::UUID ID;
	std::vector<instance::InstanceRecord> Archive;
	std::vector<util::UUID> PreviousSelection;
	bool Present = true;
};

class DuplicateInstanceCommand final : public EditorCommand
{
  public:
	DuplicateInstanceCommand(document::SceneDocument &Document, util::UUID Source);
	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] const util::UUID &GetDuplicateID() const noexcept;

  private:
	document::SceneDocument *Document = nullptr;
	util::UUID Source;
	util::UUID DuplicateID;
	std::vector<instance::InstanceRecord> SourceArchive;
	std::vector<util::UUID> PreviousSelection;
	std::unordered_map<util::UUID, util::UUID> Remap;
	bool Present = false;
};

class PasteInstanceArchiveCommand final : public EditorCommand
{
  public:
	PasteInstanceArchiveCommand(document::SceneDocument &Document, InstanceArchive Archive, util::UUID ParentOverride = {});
	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	document::SceneDocument *Document = nullptr;
	InstanceArchive Archive;
	util::UUID ParentOverride;
	std::unordered_map<util::UUID, util::UUID> Remap;
	std::vector<util::UUID> PreviousSelection;
	std::vector<util::UUID> CreatedRoots;
	bool Present = false;
};
} // namespace editor::commands
