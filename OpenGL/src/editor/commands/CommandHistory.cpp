#include "CommandHistory.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace editor::commands
{
class CommandHistory::CompositeCommand final : public EditorCommand
{
  public:
	explicit CompositeCommand(string Name) : Name(std::move(Name))
	{
		if (this->Name.empty())
			throw std::invalid_argument("Command transaction name cannot be empty");
	}

	[[nodiscard]] string_view GetName() const noexcept override
	{
		return this->Name;
	}

	void Execute() override
	{
		usize Completed = 0;
		try
		{
			for (; Completed < this->Commands.size(); ++Completed)
				this->Commands[Completed]->Execute();
		}
		catch (...)
		{
			while (Completed != 0)
				this->Commands[--Completed]->Undo();
			throw;
		}
	}

	void Undo() override
	{
		usize Index = this->Commands.size();
		try
		{
			while (Index != 0)
			{
				--Index;
				this->Commands[Index]->Undo();
			}
		}
		catch (...)
		{
			for (usize Restore = Index; Restore < this->Commands.size(); ++Restore)
				this->Commands[Restore]->Execute();
			throw;
		}
	}

	void Finalize() override
	{
		for (EditorCommandPtr &Command : this->Commands)
			Command->Finalize();
	}

	void Append(EditorCommandPtr &Command)
	{
		if (Command == nullptr)
			throw std::invalid_argument("Command transaction cannot append an empty command");
		if (!this->Commands.empty() && this->Commands.back()->TryMerge(*Command))
			return;
		this->Commands.push_back(std::move(Command));
	}

	[[nodiscard]] bool IsEmpty() const noexcept
	{
		return this->Commands.empty();
	}

  private:
	string Name;
	std::vector<EditorCommandPtr> Commands;
};

CommandHistory::CommandHistory(const usize Capacity, MutationCallback OnMutation, RevisionProvider GetRevision)
	: Capacity(Capacity), OwnerThread(std::this_thread::get_id()), OnMutation(std::move(OnMutation)), GetRevision(std::move(GetRevision))
{
	if (Capacity == 0)
		throw std::invalid_argument("CommandHistory capacity must be non-zero");
	this->UndoStack.reserve(Capacity);
	this->RedoStack.reserve(Capacity);
}

CommandHistory::~CommandHistory()
{
	try
	{
		this->Clear();
	}
	catch (...)
	{
		std::terminate();
	}
}

void CommandHistory::Execute(EditorCommandPtr Command)
{
	this->AssertOwnerThread();
	this->PrepareNewBranch();
	if (Command == nullptr)
		throw std::invalid_argument("CommandHistory cannot execute an empty command");
	Command->Execute();
	try
	{
		if (!this->Transactions.empty())
		{
			this->Transactions.back()->Append(Command);
			this->NotifyMutation();
			return;
		}
		this->Publish(Command);
		this->NotifyMutation();
	}
	catch (...)
	{
		const std::exception_ptr PublicationFailure = std::current_exception();
		if (Command != nullptr)
		{
			try
			{
				Command->Undo();
			}
			catch (...)
			{
				std::throw_with_nested(std::runtime_error("Command history publication and command rollback both failed"));
			}
		}
		std::rethrow_exception(PublicationFailure);
	}
}

void CommandHistory::Undo()
{
	this->AssertOwnerThread();
	this->VerifyRevision();
	if (!this->Transactions.empty())
		throw std::logic_error("Cannot undo while a command transaction is open");
	if (this->UndoStack.empty())
		return;

	this->UndoStack.back()->Undo();
	EditorCommandPtr Command = std::move(this->UndoStack.back());
	this->UndoStack.pop_back();
	this->RedoStack.push_back(std::move(Command));
	this->NotifyMutation();
}

void CommandHistory::Redo()
{
	this->AssertOwnerThread();
	this->VerifyRevision();
	if (!this->Transactions.empty())
		throw std::logic_error("Cannot redo while a command transaction is open");
	if (this->RedoStack.empty())
		return;

	this->RedoStack.back()->Execute();
	EditorCommandPtr Command = std::move(this->RedoStack.back());
	this->RedoStack.pop_back();
	this->UndoStack.push_back(std::move(Command));
	this->NotifyMutation();
}

void CommandHistory::BeginTransaction(string Name)
{
	this->AssertOwnerThread();
	this->PrepareNewBranch();
	this->Transactions.push_back(std::make_unique<CompositeCommand>(std::move(Name)));
}

void CommandHistory::CommitTransaction()
{
	this->AssertOwnerThread();
	this->VerifyRevision();
	if (this->Transactions.empty())
		throw std::logic_error("Cannot commit a command transaction when none is open");
	std::unique_ptr<CompositeCommand> Transaction = std::move(this->Transactions.back());
	this->Transactions.pop_back();
	if (Transaction->IsEmpty())
		return;
	EditorCommandPtr Command = std::move(Transaction);
	try
	{
		if (!this->Transactions.empty())
		{
			this->Transactions.back()->Append(Command);
			return;
		}
		this->Publish(Command);
	}
	catch (...)
	{
		if (Command != nullptr)
		{
			this->Transactions.push_back(std::unique_ptr<CompositeCommand>(static_cast<CompositeCommand *>(Command.release())));
		}
		throw;
	}
}

void CommandHistory::CancelTransaction()
{
	this->AssertOwnerThread();
	this->VerifyRevision();
	if (this->Transactions.empty())
		throw std::logic_error("Cannot cancel a command transaction when none is open");
	this->Transactions.back()->Undo();
	this->Transactions.pop_back();
	this->NotifyMutation();
}

void CommandHistory::Clear()
{
	this->AssertOwnerThread();
	for (const std::unique_ptr<CompositeCommand> &Transaction : this->Transactions)
		Transaction->Finalize();
	FinalizeCommands(this->UndoStack);
	FinalizeCommands(this->RedoStack);
	this->Transactions.clear();
	this->UndoStack.clear();
	this->RedoStack.clear();
}

void CommandHistory::SetCapacity(const usize Capacity)
{
	this->AssertOwnerThread();
	if (Capacity == 0)
		throw std::invalid_argument("CommandHistory capacity must be non-zero");
	if (!this->Transactions.empty())
		throw std::logic_error("Cannot change command history capacity while a transaction is open");
	this->Capacity = Capacity;
	if (this->UndoStack.size() > Capacity)
	{
		for (auto Iterator = this->UndoStack.begin(); Iterator != this->UndoStack.end() - static_cast<isize>(Capacity); ++Iterator)
			(*Iterator)->Finalize();
		this->UndoStack.erase(this->UndoStack.begin(), this->UndoStack.end() - static_cast<isize>(Capacity));
	}
	if (this->RedoStack.size() > Capacity)
	{
		for (auto Iterator = this->RedoStack.begin(); Iterator != this->RedoStack.end() - static_cast<isize>(Capacity); ++Iterator)
			(*Iterator)->Finalize();
		this->RedoStack.erase(this->RedoStack.begin(), this->RedoStack.end() - static_cast<isize>(Capacity));
	}
	this->UndoStack.reserve(Capacity);
	this->RedoStack.reserve(Capacity);
}

void CommandHistory::SetMutationCallback(MutationCallback OnMutation)
{
	this->AssertOwnerThread();
	this->OnMutation = std::move(OnMutation);
}

EditorTransaction::EditorTransaction(CommandHistory &History, string Name) : History(&History)
{
	this->History->BeginTransaction(std::move(Name));
	this->Active = true;
}

EditorTransaction::~EditorTransaction() noexcept
{
	if (!this->Active)
		return;
	try
	{
		this->History->CancelTransaction();
	}
	catch (...)
	{
		std::terminate();
	}
}

void EditorTransaction::Commit()
{
	if (!this->Active || this->History == nullptr)
		throw std::logic_error("Cannot commit an inactive editor transaction");
	this->History->CommitTransaction();
	this->Active = false;
}

void EditorTransaction::Cancel()
{
	if (!this->Active || this->History == nullptr)
		throw std::logic_error("Cannot cancel an inactive editor transaction");
	this->History->CancelTransaction();
	this->Active = false;
}

bool EditorTransaction::IsActive() const noexcept
{
	return this->Active;
}

bool CommandHistory::CanUndo() const noexcept
{
	return this->Transactions.empty() && !this->UndoStack.empty();
}

bool CommandHistory::CanRedo() const noexcept
{
	return this->Transactions.empty() && !this->RedoStack.empty();
}

bool CommandHistory::HasOpenTransaction() const noexcept
{
	return !this->Transactions.empty();
}

string_view CommandHistory::GetUndoName() const noexcept
{
	return this->CanUndo() ? this->UndoStack.back()->GetName() : string_view{};
}

string_view CommandHistory::GetRedoName() const noexcept
{
	return this->CanRedo() ? this->RedoStack.back()->GetName() : string_view{};
}

usize CommandHistory::GetUndoCount() const noexcept
{
	return this->UndoStack.size();
}

usize CommandHistory::GetRedoCount() const noexcept
{
	return this->RedoStack.size();
}

usize CommandHistory::GetCapacity() const noexcept
{
	return this->Capacity;
}

void CommandHistory::Publish(EditorCommandPtr &Command)
{
	FinalizeCommands(this->RedoStack);
	if (!this->UndoStack.empty() && this->UndoStack.back()->TryMerge(*Command))
	{
		this->RedoStack.clear();
		return;
	}
	this->RedoStack.clear();
	if (this->UndoStack.size() == this->Capacity)
	{
		this->UndoStack.front()->Finalize();
		this->UndoStack.erase(this->UndoStack.begin());
	}
	this->UndoStack.push_back(std::move(Command));
}

void CommandHistory::FinalizeCommands(std::vector<EditorCommandPtr> &Commands)
{
	for (EditorCommandPtr &Command : Commands)
		Command->Finalize();
}

void CommandHistory::AssertOwnerThread() const
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw std::logic_error("CommandHistory mutation must run on its owner thread");
}

void CommandHistory::PrepareNewBranch()
{
	if (!this->GetRevision)
		return;
	const uint64 CurrentRevision = this->GetRevision();
	if (!this->ExpectedRevision.has_value())
	{
		this->ExpectedRevision = CurrentRevision;
		return;
	}
	if (*this->ExpectedRevision == CurrentRevision)
		return;
	if (!this->Transactions.empty())
		throw std::logic_error("Command history cannot branch after an external mutation while a transaction is open");
	FinalizeCommands(this->UndoStack);
	FinalizeCommands(this->RedoStack);
	this->UndoStack.clear();
	this->RedoStack.clear();
	this->ExpectedRevision = CurrentRevision;
}

void CommandHistory::VerifyRevision()
{
	if (!this->GetRevision)
		return;
	const uint64 CurrentRevision = this->GetRevision();
	if (!this->ExpectedRevision.has_value())
	{
		this->ExpectedRevision = CurrentRevision;
		return;
	}
	if (*this->ExpectedRevision != CurrentRevision)
		throw std::logic_error("Command history cannot mutate a document whose revision changed outside the history");
}

void CommandHistory::NotifyMutation() noexcept
{
	try
	{
		if (this->OnMutation)
			this->OnMutation();
		if (this->GetRevision)
			this->ExpectedRevision = this->GetRevision();
	}
	catch (...)
	{
		std::terminate();
	}
}
} // namespace editor::commands
