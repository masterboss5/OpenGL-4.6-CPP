#pragma once

#include "EditorCommand.h"
#include "src/types.h"

#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace editor::commands
{
class CommandHistory final
{
  public:
	using MutationCallback = std::function<void()>;
	using RevisionProvider = std::function<uint64()>;

	explicit CommandHistory(usize Capacity = 4'096, MutationCallback OnMutation = {}, RevisionProvider GetRevision = {});
	~CommandHistory();

	void Execute(EditorCommandPtr Command);
	void Undo();
	void Redo();
	void BeginTransaction(string Name);
	void CommitTransaction();
	void CancelTransaction();
	void Clear();
	void SetCapacity(usize Capacity);
	void SetMutationCallback(MutationCallback OnMutation);

	[[nodiscard]] bool CanUndo() const noexcept;
	[[nodiscard]] bool CanRedo() const noexcept;
	[[nodiscard]] bool HasOpenTransaction() const noexcept;
	[[nodiscard]] string_view GetUndoName() const noexcept;
	[[nodiscard]] string_view GetRedoName() const noexcept;
	[[nodiscard]] usize GetUndoCount() const noexcept;
	[[nodiscard]] usize GetRedoCount() const noexcept;
	[[nodiscard]] usize GetCapacity() const noexcept;

  private:
	class CompositeCommand;

	void Publish(EditorCommandPtr &Command);
	static void FinalizeCommands(std::vector<EditorCommandPtr> &Commands);
	void AssertOwnerThread() const;
	void PrepareNewBranch();
	void VerifyRevision();
	void NotifyMutation() noexcept;

	usize Capacity = 0;
	std::vector<EditorCommandPtr> UndoStack;
	std::vector<EditorCommandPtr> RedoStack;
	std::vector<std::unique_ptr<CompositeCommand>> Transactions;
	std::thread::id OwnerThread;
	MutationCallback OnMutation;
	RevisionProvider GetRevision;
	std::optional<uint64> ExpectedRevision;
};

class EditorTransaction final
{
  public:
	EditorTransaction(CommandHistory &History, string Name);
	~EditorTransaction() noexcept;

	EditorTransaction(const EditorTransaction &) = delete;
	EditorTransaction &operator=(const EditorTransaction &) = delete;
	EditorTransaction(EditorTransaction &&) = delete;
	EditorTransaction &operator=(EditorTransaction &&) = delete;

	void Commit();
	void Cancel();
	[[nodiscard]] bool IsActive() const noexcept;

  private:
	CommandHistory *History = nullptr;
	bool Active = false;
};

// CommandHistory owns the bounded transaction stack described by the editor
// contract; retain one canonical implementation under both names.
using EditorTransactionStack = CommandHistory;
} // namespace editor::commands
