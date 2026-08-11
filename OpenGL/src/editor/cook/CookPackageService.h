#pragma once

#include "src/core/threading/TaskScheduler.h"
#include "src/editor/project/Project.h"
#include "src/resource/asset/AssetTypes.h"
#include "src/runtime/project/PackageFormat.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <atomic>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace editor::cook
{
enum class CookPackageState : uint8
{
	Idle,
	Cooking,
	Publishing,
	Completed,
	Cancelled,
	Failed
};

struct RuntimePackageFile final
{
	std::filesystem::path Source;
	std::filesystem::path Destination;
	runtime::project::PackageFileKind Kind = runtime::project::PackageFileKind::DynamicLibrary;
};

struct CookPackageSpecification final
{
	std::filesystem::path OutputDirectory;
	std::vector<RuntimePackageFile> RuntimeFiles;
	int32 CompressionLevel = 12;
	bool ReplaceExisting = true;
	bool UseIncrementalCache = true;
	bool RequireSignedPackage = false;
	string SigningKeyID;
	uint32 SigningKeyVersion = 0;
	std::filesystem::path SigningPrivateKey;
};

struct CookedContentEntry final
{
	std::filesystem::path LogicalPath;
	std::filesystem::path ArchivePath;
	uint64 ArchiveOffset = 0;
	uint64 SourceBytes = 0;
	uint64 OriginalSourceBytes = 0;
	uint64 ArchiveBytes = 0;
	uint64 ContentChecksum = 0;
	string ContentSHA256;
	resource::AssetID AssetID;
	string AssetType;
	string SourceHash;
	string Encoding = "Raw";
	string Chunk;
	std::vector<resource::AssetID> Dependencies;
};

struct CookPackageResult final
{
	util::UUID OperationID;
	util::UUID BuildID;
	std::filesystem::path PackageDirectory;
	std::vector<CookedContentEntry> Content;
	uint64 SourceBytes = 0;
	uint64 PackageBytes = 0;
};

class CookPackageException : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class CookPackageCancelledException final : public CookPackageException
{
  public:
	using CookPackageException::CookPackageException;
};

class CookPackageService final
{
  public:
	CookPackageService();
	~CookPackageService();

	CookPackageService(const CookPackageService &) = delete;
	CookPackageService &operator=(const CookPackageService &) = delete;
	CookPackageService(CookPackageService &&) = delete;
	CookPackageService &operator=(CookPackageService &&) = delete;

	void Begin(const project::Project &Project, CookPackageSpecification Specification, core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool Poll();
	void Cancel() noexcept;
	void Wait() noexcept;
	void Reset();

	[[nodiscard]] CookPackageState GetState() const noexcept;
	[[nodiscard]] float32 GetProgress() const noexcept;
	[[nodiscard]] string GetDiagnostic() const;
	[[nodiscard]] std::optional<CookPackageResult> GetResult() const;

  private:
	void Finalize();
	void FailFromCurrentException() noexcept;
	void CleanupStaging() noexcept;
	void VerifyOwnerThread() const;

	project::ProjectDescriptor ProjectDescriptor;
	project::ProjectPaths ProjectPaths;
	CookPackageSpecification Specification;
	CookPackageState State = CookPackageState::Idle;
	util::UUID OperationID;
	std::filesystem::path StagingDirectory;
	std::vector<CookedContentEntry> Entries;
	core::threading::TaskGroup Tasks;
	std::atomic<usize> CompletedTasks = 0;
	std::atomic<uint64> CompletedWorkBytes = 0;
	std::atomic<bool> CancelRequested = false;
	usize TotalTasks = 0;
	uint64 TotalWorkBytes = 0;
	string Diagnostic;
	std::optional<CookPackageResult> Result;
	std::thread::id OwnerThread;
};
} // namespace editor::cook
