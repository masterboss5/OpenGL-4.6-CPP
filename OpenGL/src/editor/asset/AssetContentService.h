#pragma once

#include "AssetRegistry.h"
#include "src/core/threading/TaskScheduler.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace editor::asset
{
enum class AssetContentOperation : uint8
{
	CreateFolder,
	CreateMaterial,
	CreateMaterialInstance,
	Move,
	Duplicate,
	Trash,
	Restore
};

enum class AssetContentServiceState : uint8
{
	Idle,
	Running,
	Completed,
	Cancelled,
	Failed
};

struct AssetContentRequest final
{
	AssetContentOperation Operation = AssetContentOperation::Move;
	std::filesystem::path Source;
	std::filesystem::path Destination;
	util::UUID TrashEntryID;
	resource::AssetID ParentAssetID;
	resource::AssetType ParentAssetType = resource::AssetType::Material;
};

struct TrashedContentEntry final
{
	util::UUID ID;
	std::filesystem::path OriginalPath;
	std::filesystem::path StoredPath;
	bool Directory = false;
	int64 TimestampMilliseconds = 0;
};

struct AssetContentResult final
{
	util::UUID OperationID;
	AssetContentOperation Operation = AssetContentOperation::Move;
	std::filesystem::path Source;
	std::filesystem::path Destination;
	bool Committed = false;
	string Diagnostic;
};

class AssetContentException : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class AssetContentCollisionException final : public AssetContentException
{
  public:
	using AssetContentException::AssetContentException;
};

class AssetContentNotFoundException final : public AssetContentException
{
  public:
	using AssetContentException::AssetContentException;
};

class AssetContentTransactionException final : public AssetContentException
{
  public:
	using AssetContentException::AssetContentException;
};

class AssetContentService final
{
  public:
	struct SharedOperationState final
	{
		std::atomic<bool> CancelRequested = false;
	};

	AssetContentService(std::filesystem::path ContentRoot, std::filesystem::path IntermediateRoot, std::filesystem::path TrashRoot);
	~AssetContentService();

	AssetContentService(const AssetContentService &) = delete;
	AssetContentService &operator=(const AssetContentService &) = delete;
	AssetContentService(AssetContentService &&) = delete;
	AssetContentService &operator=(AssetContentService &&) = delete;

	void Queue(AssetContentRequest Request, core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool Poll(core::threading::TaskScheduler &Scheduler, AssetRegistry &Registry);
	void Cancel() noexcept;
	void Wait() noexcept;
	void Reset();

	[[nodiscard]] std::vector<TrashedContentEntry> ScanTrash() const;
	[[nodiscard]] AssetContentServiceState GetState() const noexcept;
	[[nodiscard]] bool IsBusy() const noexcept;
	[[nodiscard]] std::optional<AssetContentResult> GetResult() const;

  private:
	void VerifyOwnerThread() const;
	[[nodiscard]] static AssetContentResult Execute(const std::filesystem::path &ContentRoot, const std::filesystem::path &IntermediateRoot,
													const std::filesystem::path &TrashRoot, AssetContentRequest Request,
													const std::shared_ptr<SharedOperationState> &Operation);

	std::filesystem::path ContentRoot;
	std::filesystem::path IntermediateRoot;
	std::filesystem::path TrashRoot;
	AssetContentServiceState State = AssetContentServiceState::Idle;
	std::future<AssetContentResult> Pending;
	std::shared_ptr<SharedOperationState> Operation;
	std::optional<AssetContentResult> Result;
	std::thread::id OwnerThread;
};
} // namespace editor::asset
