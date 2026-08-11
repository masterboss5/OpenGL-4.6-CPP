#pragma once

#include "AssetRegistry.h"
#include "src/core/window/WindowDialogs.h"

#include <filesystem>
#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace core
{
class Window;
}

namespace editor::asset
{
enum class ImportCollisionPolicy : uint8
{
	KeepBoth,
	Skip,
	Replace,
	Fail
};

enum class AssetImportItemStatus : uint8
{
	Imported,
	Skipped,
	Failed
};

enum class AssetImportServiceState : uint8
{
	Idle,
	Selecting,
	Importing,
	Completed,
	Cancelled,
	Failed
};

struct AssetImportProgress final
{
	uint64 CompletedBytes = 0;
	uint64 TotalBytes = 0;
	usize CompletedFiles = 0;
	usize TotalFiles = 0;

	[[nodiscard]] float32 GetFraction() const noexcept;
};

struct AssetImportRequest final
{
	std::vector<std::filesystem::path> Sources;
	std::filesystem::path DestinationDirectory;
	ImportCollisionPolicy CollisionPolicy = ImportCollisionPolicy::KeepBoth;
	bool RecursiveDirectories = true;
};

struct AssetImportItemResult final
{
	std::filesystem::path Source;
	std::filesystem::path Destination;
	AssetImportItemStatus Status = AssetImportItemStatus::Failed;
	string Diagnostic;
};

struct AssetImportBatchResult final
{
	util::UUID OperationID;
	std::vector<AssetImportItemResult> Items;
	bool Committed = false;
	string Diagnostic;

	[[nodiscard]] usize GetImportedCount() const noexcept;
	[[nodiscard]] usize GetSkippedCount() const noexcept;
	[[nodiscard]] usize GetFailedCount() const noexcept;
};

class AssetImportServiceException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class AssetImportService final
{
  public:
	struct SharedOperationState final
	{
		std::atomic<bool> CancelRequested = false;
		std::atomic<uint64> CompletedBytes = 0;
		std::atomic<uint64> TotalBytes = 0;
		std::atomic<usize> CompletedFiles = 0;
		std::atomic<usize> TotalFiles = 0;
	};

	AssetImportService(std::filesystem::path ContentRoot, std::filesystem::path IntermediateRoot);
	~AssetImportService();

	AssetImportService(const AssetImportService &) = delete;
	AssetImportService &operator=(const AssetImportService &) = delete;
	AssetImportService(AssetImportService &&) = delete;
	AssetImportService &operator=(AssetImportService &&) = delete;

	void BeginFileSelection(core::Window &Window, std::filesystem::path DestinationDirectory = {},
							ImportCollisionPolicy CollisionPolicy = ImportCollisionPolicy::KeepBoth);
	void Queue(AssetImportRequest Request, core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool Poll(core::threading::TaskScheduler &Scheduler, AssetRegistry &Registry);
	void Cancel() noexcept;
	void Wait() noexcept;
	void Reset();

	[[nodiscard]] AssetImportServiceState GetState() const noexcept;
	[[nodiscard]] bool IsBusy() const noexcept;
	[[nodiscard]] AssetImportProgress GetProgress() const noexcept;
	[[nodiscard]] const std::optional<AssetImportBatchResult> &GetResult() const noexcept;

  private:
	[[nodiscard]] static AssetImportBatchResult Import(const std::filesystem::path &ContentRoot,
													   const std::filesystem::path &IntermediateRoot, AssetImportRequest Request,
													   const std::shared_ptr<SharedOperationState> &Operation);
	void QueueSelectedFiles(core::threading::TaskScheduler &Scheduler, std::vector<std::filesystem::path> Files);

	std::filesystem::path ContentRoot;
	std::filesystem::path IntermediateRoot;
	AssetImportServiceState State = AssetImportServiceState::Idle;
	std::filesystem::path SelectionDestination;
	ImportCollisionPolicy SelectionCollisionPolicy = ImportCollisionPolicy::KeepBoth;
	core::DialogFuture<core::FileDialogSelection> PendingSelection;
	std::future<AssetImportBatchResult> PendingImport;
	std::optional<AssetImportBatchResult> Result;
	std::shared_ptr<SharedOperationState> Operation;
};
} // namespace editor::asset
