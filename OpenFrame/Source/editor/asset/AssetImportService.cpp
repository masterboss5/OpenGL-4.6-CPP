#include "AssetImportService.h"

#include "Source/core/io/SecurePath.h"
#include "Source/core/window/Window.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace editor::asset
{
namespace
{
constexpr usize MaximumImportedFiles = 100'000;

[[nodiscard]] string PathKey(const std::filesystem::path &Path)
{
	string Key = std::filesystem::absolute(Path).lexically_normal().generic_string();
	std::ranges::transform(Key, Key.begin(),
						   [](const char Character) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
	return Key;
}

struct PendingFile final
{
	std::filesystem::path Source;
	std::filesystem::path RelativeDestination;
};

struct StagedFile final
{
	std::filesystem::path Source;
	std::filesystem::path Destination;
	std::filesystem::path Staged;
	std::filesystem::path Backup;
	std::filesystem::path StagedMetadata;
	std::filesystem::path DestinationMetadata;
	std::filesystem::path BackupMetadata;
	bool ReplacesExisting = false;
	bool Published = false;
	bool MetadataPublished = false;
};

void AppendSource(const std::filesystem::path &Source, const bool Recursive, std::vector<PendingFile> &Files,
				  std::vector<AssetImportItemResult> &Failures)
{
	std::error_code Error;
	if (std::filesystem::is_symlink(std::filesystem::symlink_status(Source, Error)) && !Error)
	{
		Failures.push_back({.Source = Source, .Diagnostic = "Symbolic-link imports are not allowed"});
		return;
	}
	Error.clear();
	const std::filesystem::path Canonical = std::filesystem::canonical(Source, Error);
	if (Error)
	{
		Failures.push_back({.Source = Source, .Diagnostic = "Source is inaccessible: " + Error.message()});
		return;
	}
	if (std::filesystem::is_symlink(Canonical, Error) && !Error)
	{
		Failures.push_back({.Source = Canonical, .Diagnostic = "Symbolic-link imports are not allowed"});
		return;
	}
	if (std::filesystem::is_regular_file(Canonical, Error) && !Error)
	{
		Files.push_back({.Source = Canonical, .RelativeDestination = Canonical.filename()});
		return;
	}
	if (!std::filesystem::is_directory(Canonical, Error) || Error)
	{
		Failures.push_back({.Source = Canonical, .Diagnostic = "Source is not a regular file or directory"});
		return;
	}
	if (!Recursive)
	{
		Failures.push_back({.Source = Canonical, .Diagnostic = "Directory import requires recursive-directory support"});
		return;
	}

	std::filesystem::recursive_directory_iterator Iterator(Canonical, std::filesystem::directory_options::skip_permission_denied, Error);
	const std::filesystem::recursive_directory_iterator End;
	if (Error)
	{
		Failures.push_back({.Source = Canonical, .Diagnostic = "Could not enumerate source directory: " + Error.message()});
		return;
	}
	while (Iterator != End)
	{
		const std::filesystem::directory_entry Entry = *Iterator;
		Error.clear();
		if (Entry.is_symlink(Error))
		{
			if (Entry.is_directory(Error))
				Iterator.disable_recursion_pending();
			Failures.push_back({.Source = Entry.path(), .Diagnostic = "Symbolic-link import entry was rejected"});
		}
		else if (Entry.is_regular_file(Error) && !Error)
		{
			Files.push_back(
				{.Source = Entry.path(), .RelativeDestination = Canonical.filename() / Entry.path().lexically_relative(Canonical)});
			if (Files.size() > MaximumImportedFiles)
			{
				Failures.push_back({.Source = Canonical, .Diagnostic = "Directory import exceeds the 100000-file safety limit"});
				return;
			}
		}
		Iterator.increment(Error);
		if (Error)
		{
			Failures.push_back({.Source = Canonical, .Diagnostic = "Directory enumeration failed: " + Error.message()});
			return;
		}
	}
}

[[nodiscard]] std::filesystem::path KeepBothPath(const std::filesystem::path &Requested, std::unordered_set<string> &Reserved)
{
	if (!std::filesystem::exists(Requested) && Reserved.emplace(PathKey(Requested)).second)
		return Requested;
	for (uint32 Suffix = 1; Suffix != 0; ++Suffix)
	{
		const std::filesystem::path Candidate =
			Requested.parent_path() / (Requested.stem().string() + "-" + std::to_string(Suffix) + Requested.extension().string());
		if (!std::filesystem::exists(Candidate) && Reserved.emplace(PathKey(Candidate)).second)
			return Candidate;
	}
	throw AssetImportServiceException("Could not allocate a unique imported asset name");
}
} // namespace

usize AssetImportBatchResult::GetImportedCount() const noexcept
{
	return static_cast<usize>(std::ranges::count(this->Items, AssetImportItemStatus::Imported, &AssetImportItemResult::Status));
}

usize AssetImportBatchResult::GetSkippedCount() const noexcept
{
	return static_cast<usize>(std::ranges::count(this->Items, AssetImportItemStatus::Skipped, &AssetImportItemResult::Status));
}

usize AssetImportBatchResult::GetFailedCount() const noexcept
{
	return static_cast<usize>(std::ranges::count(this->Items, AssetImportItemStatus::Failed, &AssetImportItemResult::Status));
}

float32 AssetImportProgress::GetFraction() const noexcept
{
	if (this->TotalBytes != 0)
		return static_cast<float32>(this->CompletedBytes) / static_cast<float32>(this->TotalBytes);
	if (this->TotalFiles != 0)
		return static_cast<float32>(this->CompletedFiles) / static_cast<float32>(this->TotalFiles);
	return 0.0f;
}

AssetImportService::AssetImportService(std::filesystem::path ContentRoot, std::filesystem::path IntermediateRoot)
	: ContentRoot(std::filesystem::absolute(std::move(ContentRoot)).lexically_normal()),
	  IntermediateRoot(std::filesystem::absolute(std::move(IntermediateRoot)).lexically_normal())
{
}

AssetImportService::~AssetImportService()
{
	this->Cancel();
	this->Wait();
}

void AssetImportService::BeginFileSelection(core::Window &Window, std::filesystem::path DestinationDirectory,
											const ImportCollisionPolicy CollisionPolicy)
{
	if (this->IsBusy())
		throw AssetImportServiceException("An asset import operation is already active");
	if (DestinationDirectory.is_absolute())
		throw std::invalid_argument("Asset import destination must be relative to project Content");
	this->SelectionDestination = std::move(DestinationDirectory);
	this->SelectionCollisionPolicy = CollisionPolicy;
	this->Result.reset();
	this->PendingSelection = Window.BeginFileDialog({.Operation = core::FileDialogOperation::OpenFiles,
													 .Title = "Import Assets",
													 .Filters = {{"Supported Assets",
																  {"*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp", "*.hdr", "*.gltf", "*.glb",
																   "*.obj", "*.fbx", "*.dae", "*.glsl", "*.vert", "*.frag", "*.comp"}},
																 {"All Files", {"*.*"}}},
													 .ShowHidden = false,
													 .AddToRecent = false,
													 .RequireExistingPath = true});
	this->State = AssetImportServiceState::Selecting;
}

void AssetImportService::Queue(AssetImportRequest Request, core::threading::TaskScheduler &Scheduler)
{
	if (this->IsBusy())
		throw AssetImportServiceException("An asset import operation is already active");
	if (Request.Sources.empty())
		throw std::invalid_argument("Asset import request requires at least one source");
	if (Request.DestinationDirectory.is_absolute())
		throw std::invalid_argument("Asset import destination must be relative to project Content");
	this->Result.reset();
	this->Operation = std::make_shared<SharedOperationState>();
	const std::filesystem::path Content = this->ContentRoot;
	const std::filesystem::path Intermediate = this->IntermediateRoot;
	const std::shared_ptr<SharedOperationState> Operation = this->Operation;
	this->PendingImport = Scheduler.Submit([Content, Intermediate, Request = std::move(Request), Operation]() mutable
										   { return AssetImportService::Import(Content, Intermediate, std::move(Request), Operation); },
										   core::threading::TaskPriority::Background);
	this->State = AssetImportServiceState::Importing;
}

bool AssetImportService::Poll(core::threading::TaskScheduler &Scheduler, AssetRegistry &Registry)
{
	if (this->State == AssetImportServiceState::Selecting)
	{
		if (!this->PendingSelection.IsReady())
			return false;
		core::DialogResult<core::FileDialogSelection> Selection = this->PendingSelection.Take();
		if (!Selection.Accepted())
		{
			this->State = AssetImportServiceState::Idle;
			return true;
		}
		this->QueueSelectedFiles(Scheduler, std::move(Selection.Value->Paths));
		return true;
	}
	if (this->State != AssetImportServiceState::Importing || !this->PendingImport.valid() ||
		this->PendingImport.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		return false;
	}
	try
	{
		this->Result = this->PendingImport.get();
		this->State = this->Result->Committed
						  ? AssetImportServiceState::Completed
						  : (this->Operation != nullptr && this->Operation->CancelRequested.load(std::memory_order_acquire)
								 ? AssetImportServiceState::Cancelled
								 : AssetImportServiceState::Failed);
		if (this->Result->Committed)
			Registry.RequestRefresh(Scheduler, true);
	}
	catch (const std::exception &Exception)
	{
		this->Result = AssetImportBatchResult{.OperationID = util::UUID::GenerateRandomUUID(), .Diagnostic = Exception.what()};
		this->State = AssetImportServiceState::Failed;
	}
	catch (...)
	{
		this->Result = AssetImportBatchResult{.OperationID = util::UUID::GenerateRandomUUID(),
											  .Diagnostic = "Asset import failed with a non-standard exception"};
		this->State = AssetImportServiceState::Failed;
	}
	return true;
}

void AssetImportService::Cancel() noexcept
{
	if (this->Operation != nullptr)
		this->Operation->CancelRequested.store(true, std::memory_order_release);
}

void AssetImportService::Wait() noexcept
{
	if (!this->PendingImport.valid())
		return;
	try
	{
		this->Result = this->PendingImport.get();
		this->State = this->Result->Committed
						  ? AssetImportServiceState::Completed
						  : (this->Operation != nullptr && this->Operation->CancelRequested.load(std::memory_order_acquire)
								 ? AssetImportServiceState::Cancelled
								 : AssetImportServiceState::Failed);
	}
	catch (...)
	{
		this->State = AssetImportServiceState::Failed;
	}
}

void AssetImportService::Reset()
{
	if (this->IsBusy())
		throw AssetImportServiceException("Cannot reset an active asset import operation");
	this->Result.reset();
	this->Operation.reset();
	this->State = AssetImportServiceState::Idle;
}

AssetImportServiceState AssetImportService::GetState() const noexcept
{
	return this->State;
}

bool AssetImportService::IsBusy() const noexcept
{
	return this->State == AssetImportServiceState::Selecting || this->State == AssetImportServiceState::Importing;
}

AssetImportProgress AssetImportService::GetProgress() const noexcept
{
	if (this->Operation == nullptr)
		return {};
	return {.CompletedBytes = this->Operation->CompletedBytes.load(std::memory_order_acquire),
			.TotalBytes = this->Operation->TotalBytes.load(std::memory_order_acquire),
			.CompletedFiles = this->Operation->CompletedFiles.load(std::memory_order_acquire),
			.TotalFiles = this->Operation->TotalFiles.load(std::memory_order_acquire)};
}

const std::optional<AssetImportBatchResult> &AssetImportService::GetResult() const noexcept
{
	return this->Result;
}

AssetImportBatchResult AssetImportService::Import(const std::filesystem::path &ContentRoot, const std::filesystem::path &IntermediateRoot,
												  AssetImportRequest Request, const std::shared_ptr<SharedOperationState> &Operation)
{
	AssetImportBatchResult Result{.OperationID = util::UUID::GenerateRandomUUID()};
	std::filesystem::path DestinationRoot;
	try
	{
		if (Request.DestinationDirectory.empty() || Request.DestinationDirectory == ".")
		{
			core::io::SecurePath::VerifyContained(ContentRoot, ContentRoot, "Asset import destination");
			DestinationRoot = ContentRoot;
		}
		else
			DestinationRoot = core::io::SecurePath::ResolveWithin(ContentRoot, Request.DestinationDirectory, "Asset import destination");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		Result.Diagnostic = Exception.what();
		return Result;
	}
	std::vector<PendingFile> Files;
	Files.reserve(Request.Sources.size());
	for (const std::filesystem::path &Source : Request.Sources)
	{
		AppendSource(Source, Request.RecursiveDirectories, Files, Result.Items);
		if (Files.size() > MaximumImportedFiles)
		{
			Result.Items.push_back({.Source = Source, .Diagnostic = "Asset import exceeds the 100000-file safety limit"});
			break;
		}
	}
	if (!Result.Items.empty() || Files.empty())
	{
		Result.Diagnostic = Files.empty() ? "Asset import contains no regular files" : "Asset import source validation failed";
		return Result;
	}
	Operation->TotalFiles.store(Files.size(), std::memory_order_release);
	std::error_code Error;
	uint64 TotalBytes = 0;
	for (const PendingFile &File : Files)
	{
		Error.clear();
		const uint64 Size = std::filesystem::file_size(File.Source, Error);
		if (Error || Size > std::numeric_limits<uint64>::max() - TotalBytes)
		{
			Result.Diagnostic = "Could not determine a safe total import size";
			return Result;
		}
		TotalBytes += Size;
	}
	Operation->TotalBytes.store(TotalBytes, std::memory_order_release);

	const std::filesystem::path StagingRoot = IntermediateRoot / "ImportStaging" / Result.OperationID.ToString();
	const std::filesystem::path PayloadRoot = StagingRoot / "Payload";
	const std::filesystem::path BackupRoot = StagingRoot / "Backup";
	try
	{
		core::io::SecurePath::CreateDirectoriesWithin(IntermediateRoot, PayloadRoot.lexically_relative(IntermediateRoot),
													  "Asset import staging");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		Result.Diagnostic = Exception.what();
		return Result;
	}
	struct Cleanup final
	{
		std::filesystem::path Root;
		std::filesystem::path Relative;
		~Cleanup()
		{
			try
			{
				if (std::filesystem::exists(this->Root / this->Relative))
					core::io::SecurePath::RemoveWithin(this->Root, this->Relative, true, "Asset import staging cleanup");
			}
			catch (...)
			{
			}
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{IntermediateRoot, StagingRoot.lexically_relative(IntermediateRoot)};

	std::unordered_set<string> ReservedDestinations;
	std::vector<StagedFile> Staged;
	Staged.reserve(Files.size());
	for (const PendingFile &File : Files)
	{
		if (Operation->CancelRequested.load(std::memory_order_acquire))
		{
			Result.Diagnostic = "Asset import was cancelled before publication";
			return Result;
		}
		std::filesystem::path Destination = DestinationRoot / File.RelativeDestination;
		const bool Exists = std::filesystem::exists(Destination, Error);
		if (Error)
		{
			Result.Items.push_back({.Source = File.Source,
									.Destination = Destination,
									.Diagnostic = "Could not inspect import destination: " + Error.message()});
			break;
		}
		if (Exists || ReservedDestinations.contains(PathKey(Destination)))
		{
			if (Request.CollisionPolicy == ImportCollisionPolicy::Skip)
			{
				Result.Items.push_back({.Source = File.Source,
										.Destination = Destination,
										.Status = AssetImportItemStatus::Skipped,
										.Diagnostic = "Destination already exists"});
				Error.clear();
				const uint64 SkippedBytes = std::filesystem::file_size(File.Source, Error);
				if (!Error)
					Operation->CompletedBytes.fetch_add(SkippedBytes, std::memory_order_release);
				Operation->CompletedFiles.fetch_add(1, std::memory_order_release);
				continue;
			}
			if (Request.CollisionPolicy == ImportCollisionPolicy::Fail)
			{
				Result.Items.push_back({.Source = File.Source, .Destination = Destination, .Diagnostic = "Destination already exists"});
				break;
			}
			if (Request.CollisionPolicy == ImportCollisionPolicy::KeepBoth)
				Destination = KeepBothPath(Destination, ReservedDestinations);
			else
				ReservedDestinations.emplace(PathKey(Destination));
		}
		else
		{
			ReservedDestinations.emplace(PathKey(Destination));
		}

		const std::filesystem::path RelativeFinal = Destination.lexically_relative(ContentRoot);
		const std::filesystem::path StagedPath = PayloadRoot / RelativeFinal;
		string CopyDiagnostic;
		try
		{
			core::io::SecurePath::CopyWithin(File.Source.parent_path(), File.Source.filename(), IntermediateRoot,
											 StagedPath.lexically_relative(IntermediateRoot), false, false, "Asset import staging copy");
			const uint64 CopiedBytes = std::filesystem::file_size(File.Source);
			Operation->CompletedBytes.fetch_add(CopiedBytes, std::memory_order_release);
		}
		catch (const std::exception &Exception)
		{
			CopyDiagnostic = Exception.what();
		}
		if (!CopyDiagnostic.empty())
		{
			Result.Items.push_back(
				{.Source = File.Source, .Destination = Destination, .Diagnostic = "Could not stage imported file: " + CopyDiagnostic});
			break;
		}
		StagedFile StagedEntry{.Source = File.Source,
							   .Destination = Destination,
							   .Staged = StagedPath,
							   .Backup = BackupRoot / RelativeFinal,
							   .ReplacesExisting = Exists && Request.CollisionPolicy == ImportCollisionPolicy::Replace};
		const std::optional<string> AssetType = AssetMetadataStore::InferAssetTypeName(Destination);
		if (AssetType.has_value())
		{
			StagedEntry.StagedMetadata = AssetMetadataStore::GetSidecarPath(StagedPath);
			StagedEntry.DestinationMetadata = AssetMetadataStore::GetSidecarPath(Destination);
			StagedEntry.BackupMetadata = AssetMetadataStore::GetSidecarPath(StagedEntry.Backup);
			string MetadataDiagnostic;
			std::optional<AssetMetadata> Metadata;
			if (StagedEntry.ReplacesExisting && std::filesystem::is_regular_file(StagedEntry.DestinationMetadata))
				Metadata = AssetMetadataStore::TryLoad(StagedEntry.DestinationMetadata, MetadataDiagnostic);
			if (!Metadata.has_value())
				Metadata = AssetMetadataStore::Create(StagedPath, "/Game/" + RelativeFinal.generic_string(), *AssetType);
			Metadata->AssetType = *AssetType;
			Metadata->VirtualSource = "/Game/" + RelativeFinal.generic_string();
			Metadata->PhysicalSourceIdentity = AssetMetadataStore::CalculatePhysicalSourceIdentity(StagedPath);
			Metadata->SourceHash = AssetMetadataStore::CalculateSourceHash(StagedPath);
			AssetMetadataStore::Save(*Metadata, StagedEntry.StagedMetadata);
		}
		Staged.push_back(std::move(StagedEntry));
		Operation->CompletedFiles.fetch_add(1, std::memory_order_release);
	}

	if (Result.GetFailedCount() != 0)
	{
		Result.Diagnostic = "Asset import was rolled back during staging";
		return Result;
	}

	if (Operation->CancelRequested.load(std::memory_order_acquire))
	{
		Result.Diagnostic = "Asset import was cancelled before publication";
		return Result;
	}
	string PublicationError;
	for (StagedFile &File : Staged)
	{
		try
		{
			if (File.ReplacesExisting)
			{
				core::io::SecurePath::MoveWithin(ContentRoot, File.Destination.lexically_relative(ContentRoot), IntermediateRoot,
												 File.Backup.lexically_relative(IntermediateRoot), false, "Asset import payload backup");
				if (std::filesystem::is_regular_file(File.DestinationMetadata))
					core::io::SecurePath::MoveWithin(ContentRoot, File.DestinationMetadata.lexically_relative(ContentRoot),
													 IntermediateRoot, File.BackupMetadata.lexically_relative(IntermediateRoot), false,
													 "Asset import metadata backup");
			}
			core::io::SecurePath::MoveWithin(IntermediateRoot, File.Staged.lexically_relative(IntermediateRoot), ContentRoot,
											 File.Destination.lexically_relative(ContentRoot), false, "Asset import payload publication");
			File.Published = true;
			if (!File.StagedMetadata.empty())
			{
				core::io::SecurePath::MoveWithin(IntermediateRoot, File.StagedMetadata.lexically_relative(IntermediateRoot), ContentRoot,
												 File.DestinationMetadata.lexically_relative(ContentRoot), false,
												 "Asset import metadata publication");
				File.MetadataPublished = true;
			}
		}
		catch (const std::exception &Exception)
		{
			PublicationError = Exception.what();
			if (File.ReplacesExisting)
			{
				try
				{
					if (std::filesystem::exists(File.Backup))
						core::io::SecurePath::MoveWithin(IntermediateRoot, File.Backup.lexically_relative(IntermediateRoot), ContentRoot,
														 File.Destination.lexically_relative(ContentRoot), false,
														 "Asset import payload restore");
				}
				catch (...)
				{
				}
			}
			break;
		}
	}

	if (!PublicationError.empty())
	{
		string RollbackDiagnostic;
		for (usize Index = Staged.size(); Index-- > 0;)
		{
			StagedFile &File = Staged[Index];
			try
			{
				if (File.MetadataPublished)
					core::io::SecurePath::MoveWithin(ContentRoot, File.DestinationMetadata.lexically_relative(ContentRoot),
													 IntermediateRoot, File.StagedMetadata.lexically_relative(IntermediateRoot), false,
													 "Asset import metadata rollback");
				if (File.Published)
					core::io::SecurePath::MoveWithin(ContentRoot, File.Destination.lexically_relative(ContentRoot), IntermediateRoot,
													 File.Staged.lexically_relative(IntermediateRoot), false,
													 "Asset import payload rollback");
				if (File.ReplacesExisting && std::filesystem::exists(File.Backup))
					core::io::SecurePath::MoveWithin(IntermediateRoot, File.Backup.lexically_relative(IntermediateRoot), ContentRoot,
													 File.Destination.lexically_relative(ContentRoot), false,
													 "Asset import payload restore");
				if (File.ReplacesExisting && std::filesystem::exists(File.BackupMetadata))
					core::io::SecurePath::MoveWithin(IntermediateRoot, File.BackupMetadata.lexically_relative(IntermediateRoot),
													 ContentRoot, File.DestinationMetadata.lexically_relative(ContentRoot), false,
													 "Asset import metadata restore");
			}
			catch (const std::exception &Exception)
			{
				if (RollbackDiagnostic.empty())
					RollbackDiagnostic = Exception.what();
			}
		}
		Result.Items.push_back(
			{.Status = AssetImportItemStatus::Failed, .Diagnostic = "Could not publish staged import: " + PublicationError});
		Result.Diagnostic = RollbackDiagnostic.empty() ? "Asset import publication failed and was rolled back"
													   : "Asset import rollback was incomplete: " + RollbackDiagnostic;
		return Result;
	}

	for (const StagedFile &File : Staged)
		Result.Items.push_back({.Source = File.Source, .Destination = File.Destination, .Status = AssetImportItemStatus::Imported});
	Result.Committed = true;
	Result.Diagnostic = "Imported " + std::to_string(Result.GetImportedCount()) + " asset files";
	return Result;
}

void AssetImportService::QueueSelectedFiles(core::threading::TaskScheduler &Scheduler, std::vector<std::filesystem::path> Files)
{
	this->State = AssetImportServiceState::Idle;
	this->Queue({.Sources = std::move(Files),
				 .DestinationDirectory = this->SelectionDestination,
				 .CollisionPolicy = this->SelectionCollisionPolicy},
				Scheduler);
}
} // namespace editor::asset
