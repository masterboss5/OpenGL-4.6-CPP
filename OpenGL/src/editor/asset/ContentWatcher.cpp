#include "ContentWatcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <iterator>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace editor::asset
{
class ContentWatcher::Implementation final
{
  public:
	static constexpr usize MaximumPendingChanges = 8'192;

	[[nodiscard]] static bool SamePath(const std::filesystem::path &Left, const std::filesystem::path &Right) noexcept
	{
		return _wcsicmp(Left.native().c_str(), Right.native().c_str()) == 0;
	}

	[[nodiscard]] static std::filesystem::path GetRelativePath(const FILE_NOTIFY_INFORMATION &Information)
	{
		const std::wstring Name(Information.FileName, Information.FileNameLength / sizeof(wchar_t));
		return std::filesystem::path(Name).lexically_normal();
	}

	void MarkOverflow(string DiagnosticText)
	{
		{
			std::scoped_lock Lock(this->DiagnosticMutex);
			this->Diagnostic = std::move(DiagnosticText);
		}
		{
			std::scoped_lock Lock(this->ChangeMutex);
			this->PendingChanges.clear();
		}
		this->OverflowCount.fetch_add(1, std::memory_order_relaxed);
		this->ChangeGeneration.fetch_add(1, std::memory_order_release);
	}

	[[nodiscard]] bool QueueChange(ContentChange Change)
	{
		Change.RelativePath = Change.RelativePath.lexically_normal();
		Change.PreviousPath = Change.PreviousPath.lexically_normal();
		if (Change.RelativePath.empty())
			return false;
		bool CapacityExceeded = false;
		{
			std::scoped_lock Lock(this->ChangeMutex);
			if (Change.Kind == ContentChangeKind::Renamed)
			{
				std::erase_if(this->PendingChanges,
							  [&Change](const ContentChange &Existing)
							  {
								  return SamePath(Existing.RelativePath, Change.RelativePath) ||
										 SamePath(Existing.RelativePath, Change.PreviousPath) ||
										 SamePath(Existing.PreviousPath, Change.RelativePath) ||
										 SamePath(Existing.PreviousPath, Change.PreviousPath);
							  });
			}
			else
			{
				const auto Existing = std::ranges::find_if(this->PendingChanges,
														   [&Change](const ContentChange &Candidate)
														   {
															   return SamePath(Candidate.RelativePath, Change.RelativePath) ||
																	  SamePath(Candidate.PreviousPath, Change.RelativePath);
														   });
				if (Existing != this->PendingChanges.end())
				{
					if (Existing->Kind == ContentChangeKind::Added && Change.Kind == ContentChangeKind::Modified)
						return true;
					if (Existing->Kind == ContentChangeKind::Added && Change.Kind == ContentChangeKind::Removed)
					{
						this->PendingChanges.erase(Existing);
						return true;
					}
					if (Existing->Kind == ContentChangeKind::Renamed && Change.Kind == ContentChangeKind::Modified)
						return true;
					Existing->Kind = Change.Kind;
					Existing->RelativePath = std::move(Change.RelativePath);
					return true;
				}
			}
			if (this->PendingChanges.size() >= MaximumPendingChanges)
				CapacityExceeded = true;
			else
				this->PendingChanges.push_back(std::move(Change));
		}
		if (CapacityExceeded)
		{
			this->MarkOverflow("Content watcher change queue reached its bounded capacity; scheduling a complete registry rescan");
			return false;
		}
		return true;
	}

	[[nodiscard]] bool ParseNotifications(const uint8 *Buffer, const DWORD BytesReturned)
	{
		constexpr usize HeaderSize = offsetof(FILE_NOTIFY_INFORMATION, FileName);
		usize Offset = 0;
		bool Changed = false;
		std::optional<std::filesystem::path> PendingRename;
		while (Offset < static_cast<usize>(BytesReturned))
		{
			const usize Remaining = static_cast<usize>(BytesReturned) - Offset;
			if (Remaining < HeaderSize || Remaining < HeaderSize + sizeof(wchar_t))
				return false;
			const auto *Information = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(Buffer + Offset);
			if (Information->FileNameLength == 0 || Information->FileNameLength % sizeof(wchar_t) != 0 ||
				Information->FileNameLength > Remaining - HeaderSize)
				return false;
			const std::filesystem::path Path = GetRelativePath(*Information);
			if (Path.empty())
				return false;
			switch (Information->Action)
			{
			case FILE_ACTION_ADDED:
				if (PendingRename.has_value())
				{
					Changed |= this->QueueChange({.Kind = ContentChangeKind::Removed, .RelativePath = std::move(*PendingRename)});
					PendingRename.reset();
				}
				Changed |= this->QueueChange({.Kind = ContentChangeKind::Added, .RelativePath = Path});
				break;
			case FILE_ACTION_REMOVED:
				if (PendingRename.has_value())
				{
					Changed |= this->QueueChange({.Kind = ContentChangeKind::Removed, .RelativePath = std::move(*PendingRename)});
					PendingRename.reset();
				}
				Changed |= this->QueueChange({.Kind = ContentChangeKind::Removed, .RelativePath = Path});
				break;
			case FILE_ACTION_MODIFIED:
				if (PendingRename.has_value())
				{
					Changed |= this->QueueChange({.Kind = ContentChangeKind::Removed, .RelativePath = std::move(*PendingRename)});
					PendingRename.reset();
				}
				Changed |= this->QueueChange({.Kind = ContentChangeKind::Modified, .RelativePath = Path});
				break;
			case FILE_ACTION_RENAMED_OLD_NAME:
				if (PendingRename.has_value())
					Changed |= this->QueueChange({.Kind = ContentChangeKind::Removed, .RelativePath = std::move(*PendingRename)});
				PendingRename = Path;
				break;
			case FILE_ACTION_RENAMED_NEW_NAME:
				if (PendingRename.has_value())
				{
					Changed |= this->QueueChange(
						{.Kind = ContentChangeKind::Renamed, .RelativePath = Path, .PreviousPath = std::move(*PendingRename)});
					PendingRename.reset();
				}
				else
					Changed |= this->QueueChange({.Kind = ContentChangeKind::Added, .RelativePath = Path});
				break;
			default:
				return false;
			}
			if (Information->NextEntryOffset == 0)
			{
				Offset = static_cast<usize>(BytesReturned);
				break;
			}
			if (Information->NextEntryOffset < HeaderSize + Information->FileNameLength || Information->NextEntryOffset > Remaining)
				return false;
			Offset += Information->NextEntryOffset;
		}
		if (PendingRename.has_value())
			Changed |= this->QueueChange({.Kind = ContentChangeKind::Removed, .RelativePath = std::move(*PendingRename)});
		return Changed;
	}

	void Run(const std::stop_token StopToken)
	{
		std::array<uint8, 64U * 1'024U> Buffer{};
		while (!StopToken.stop_requested())
		{
			DWORD BytesReturned = 0;
			const BOOL Result =
				ReadDirectoryChangesW(this->Directory, Buffer.data(), static_cast<DWORD>(Buffer.size()), TRUE,
									  FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
										  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES,
									  &BytesReturned, nullptr, nullptr);
			if (Result == FALSE)
			{
				const DWORD Error = GetLastError();
				if (StopToken.stop_requested() || Error == ERROR_OPERATION_ABORTED)
					break;
				if (Error == ERROR_NOTIFY_ENUM_DIR || Error == ERROR_MORE_DATA)
				{
					this->MarkOverflow("Content watcher notification buffer overflowed; scheduling a complete registry rescan");
					continue;
				}
				this->MarkOverflow("Content watcher failed with native error " + std::to_string(static_cast<uint32>(Error)));
				break;
			}
			if (BytesReturned == 0)
			{
				this->MarkOverflow("Content watcher returned an empty notification buffer; scheduling a complete registry rescan");
				continue;
			}
			const bool Changed = this->ParseNotifications(Buffer.data(), BytesReturned);
			if (!Changed)
			{
				this->MarkOverflow("Content watcher received malformed change records; scheduling a complete registry rescan");
				continue;
			}
			this->ChangeGeneration.fetch_add(1, std::memory_order_release);
		}
	}

	void DrainChangesInto(std::vector<ContentChange> &Result)
	{
		std::scoped_lock Lock(this->ChangeMutex);
		Result.clear();
		if (Result.capacity() < this->PendingChanges.size())
			Result.reserve(this->PendingChanges.size());
		Result.insert(Result.end(), std::make_move_iterator(this->PendingChanges.begin()),
					  std::make_move_iterator(this->PendingChanges.end()));
		this->PendingChanges.clear();
	}

	HANDLE Directory = INVALID_HANDLE_VALUE;
	std::jthread Thread;
	std::atomic<uint64> ChangeGeneration = 1;
	std::atomic<uint64> OverflowCount = 0;
	mutable std::mutex DiagnosticMutex;
	std::mutex ChangeMutex;
	std::vector<ContentChange> PendingChanges;
	string Diagnostic;
};

ContentWatcher::ContentWatcher() : State(std::make_unique<Implementation>())
{
}

ContentWatcher::~ContentWatcher()
{
	this->Stop();
}

void ContentWatcher::Start(const std::filesystem::path &Root)
{
	if (this->IsRunning())
		throw ContentWatcherException("Content watcher is already running");
	std::error_code Error;
	const std::filesystem::path CanonicalRoot = std::filesystem::canonical(Root, Error);
	if (Error || !std::filesystem::is_directory(CanonicalRoot, Error) || Error)
		throw ContentWatcherException("Content watcher root does not exist or is inaccessible: '" + Root.string() + "'");
	{
		std::scoped_lock Lock(this->State->ChangeMutex);
		this->State->PendingChanges.clear();
	}
	{
		std::scoped_lock Lock(this->State->DiagnosticMutex);
		this->State->Diagnostic.clear();
	}
	this->State->Directory = CreateFileW(CanonicalRoot.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
										 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (this->State->Directory == INVALID_HANDLE_VALUE)
		throw ContentWatcherException("Could not open content root for change notifications, native error " +
									  std::to_string(static_cast<uint32>(GetLastError())));
	this->State->Thread = std::jthread([this](const std::stop_token StopToken) { this->State->Run(StopToken); });
}

void ContentWatcher::Stop() noexcept
{
	if (!this->State->Thread.joinable())
		return;
	this->State->Thread.request_stop();
	if (this->State->Directory != INVALID_HANDLE_VALUE)
		(void)CancelIoEx(this->State->Directory, nullptr);
	this->State->Thread.join();
	if (this->State->Directory != INVALID_HANDLE_VALUE)
	{
		(void)CloseHandle(this->State->Directory);
		this->State->Directory = INVALID_HANDLE_VALUE;
	}
}

bool ContentWatcher::IsRunning() const noexcept
{
	return this->State->Thread.joinable();
}

uint64 ContentWatcher::GetChangeGeneration() const noexcept
{
	return this->State->ChangeGeneration.load(std::memory_order_acquire);
}

uint64 ContentWatcher::GetOverflowCount() const noexcept
{
	return this->State->OverflowCount.load(std::memory_order_relaxed);
}

string ContentWatcher::GetDiagnostic() const
{
	std::scoped_lock Lock(this->State->DiagnosticMutex);
	return this->State->Diagnostic;
}

void ContentWatcher::DrainChangesInto(std::vector<ContentChange> &Result)
{
	this->State->DrainChangesInto(Result);
}
} // namespace editor::asset
