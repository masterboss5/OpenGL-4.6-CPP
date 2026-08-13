#include "AssetContentService.h"

#include "AssetMetadata.h"
#include "Source/core/io/SecurePath.h"
#include "Source/editor/material/MaterialDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace editor::asset
{
namespace
{
using Json = nlohmann::json;
constexpr uint64 MaximumContentDocumentBytes = 64U * 1024U * 1024U;

[[nodiscard]] Json ReadJsonWithin(const std::filesystem::path &Root, const std::filesystem::path &Relative, const string_view Role)
{
	try
	{
		const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileWithin(Root, Relative, MaximumContentDocumentBytes, Role);
		return Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
	}
	catch (const std::exception &Exception)
	{
		throw AssetContentTransactionException(string(Role) + " could not be read securely: " + Exception.what());
	}
}

struct MetadataRecord final
{
	std::filesystem::path SourceRelativePath;
	AssetMetadata Metadata;
};

class StagingScope final
{
  public:
	StagingScope(std::filesystem::path Root, std::filesystem::path Relative)
		: Root(std::move(Root)), Relative(std::move(Relative)), Path(this->Root / this->Relative)
	{
	}

	~StagingScope()
	{
		try
		{
			if (std::filesystem::exists(this->Path))
				core::io::SecurePath::RemoveWithin(this->Root, this->Relative, true, "Content staging cleanup");
		}
		catch (...)
		{
		}
	}

	[[nodiscard]] const std::filesystem::path &GetPath() const noexcept
	{
		return this->Path;
	}

  private:
	std::filesystem::path Root;
	std::filesystem::path Relative;
	std::filesystem::path Path;
};

[[nodiscard]] bool IsMetadataPath(const std::filesystem::path &Path)
{
	return Path.extension() == ".assetmeta";
}

[[nodiscard]] std::filesystem::path NormalizeRelative(const std::filesystem::path &Path, const string_view Role)
{
	if (Path.empty() || Path.is_absolute())
		throw std::invalid_argument(string(Role) + " must be a non-empty path relative to project Content");
	const std::filesystem::path Result = Path.lexically_normal();
	if (Result.empty() || Result == "." || *Result.begin() == ".." || IsMetadataPath(Result))
		throw std::invalid_argument(string(Role) + " is invalid or escapes project Content");
	return Result;
}

[[nodiscard]] std::filesystem::path ResolveWithin(const std::filesystem::path &Root, const std::filesystem::path &Relative,
												  const string_view Role)
{
	return core::io::SecurePath::ResolveWithin(Root, NormalizeRelative(Relative, Role), Role);
}

[[nodiscard]] string MakeVirtualPath(const std::filesystem::path &Relative)
{
	return "/Game/" + Relative.generic_string();
}

[[nodiscard]] std::filesystem::path SourceForSidecar(const std::filesystem::path &Sidecar)
{
	const string Text = Sidecar.string();
	constexpr string_view Suffix = ".assetmeta";
	if (!Text.ends_with(Suffix))
		throw AssetContentTransactionException("Metadata sidecar path has an invalid suffix");
	return Text.substr(0, Text.size() - Suffix.size());
}

[[nodiscard]] std::vector<MetadataRecord> ReadMetadataTree(const std::filesystem::path &Source, const std::filesystem::path &SourceRoot)
{
	std::vector<std::filesystem::path> Sidecars;
	const std::filesystem::path DirectSidecar = AssetMetadataStore::GetSidecarPath(Source);
	if (std::filesystem::is_regular_file(DirectSidecar))
		Sidecars.push_back(DirectSidecar);
	if (std::filesystem::is_directory(Source))
	{
		std::error_code Error;
		for (std::filesystem::recursive_directory_iterator
				 Iterator(Source, std::filesystem::directory_options::skip_permission_denied, Error),
			 End;
			 !Error && Iterator != End; Iterator.increment(Error))
		{
			if (Iterator->is_regular_file(Error) && !Error && IsMetadataPath(Iterator->path()))
				Sidecars.push_back(Iterator->path());
		}
		if (Error)
			throw AssetContentTransactionException("Could not enumerate asset metadata: " + Error.message());
	}

	std::vector<MetadataRecord> Result;
	Result.reserve(Sidecars.size());
	for (const std::filesystem::path &Sidecar : Sidecars)
	{
		string Diagnostic;
		std::optional<AssetMetadata> Metadata = AssetMetadataStore::TryLoad(Sidecar, Diagnostic);
		if (!Metadata.has_value())
			throw AssetContentTransactionException(Diagnostic);
		Result.push_back(
			{.SourceRelativePath = SourceForSidecar(Sidecar).lexically_relative(SourceRoot), .Metadata = std::move(*Metadata)});
	}
	return Result;
}

void CopyPayload(const std::filesystem::path &SourceRoot, const std::filesystem::path &SourceRelative,
				 const std::filesystem::path &DestinationRoot, const std::filesystem::path &DestinationRelative)
{
	const std::filesystem::path Source = ResolveWithin(SourceRoot, SourceRelative, "Content copy source");
	const std::filesystem::path Destination =
		core::io::SecurePath::ResolveWithin(DestinationRoot, DestinationRelative, "Content copy destination");
	core::io::SecurePath::CopyWithin(SourceRoot, SourceRelative, DestinationRoot, DestinationRelative,
									 std::filesystem::is_directory(Source), false, "Content payload copy");
	const std::filesystem::path SourceSidecar = AssetMetadataStore::GetSidecarPath(Source);
	if (std::filesystem::is_regular_file(SourceSidecar))
	{
		try
		{
			core::io::SecurePath::CopyWithin(SourceRoot, SourceSidecar.lexically_relative(SourceRoot), DestinationRoot,
											 AssetMetadataStore::GetSidecarPath(Destination).lexically_relative(DestinationRoot), false,
											 false, "Content metadata copy");
		}
		catch (...)
		{
			try
			{
				core::io::SecurePath::RemoveWithin(DestinationRoot, DestinationRelative, true, "Content copy rollback");
			}
			catch (...)
			{
			}
			throw;
		}
	}
}

void RenamePayload(const std::filesystem::path &SourceRoot, const std::filesystem::path &SourceRelative,
				   const std::filesystem::path &DestinationRoot, const std::filesystem::path &DestinationRelative)
{
	const std::filesystem::path Source = ResolveWithin(SourceRoot, SourceRelative, "Content move source");
	const std::filesystem::path Destination =
		core::io::SecurePath::ResolveWithin(DestinationRoot, DestinationRelative, "Content move destination");
	core::io::SecurePath::MoveWithin(SourceRoot, SourceRelative, DestinationRoot, DestinationRelative, false, "Content payload move");
	const std::filesystem::path SourceSidecar = AssetMetadataStore::GetSidecarPath(Source);
	if (!std::filesystem::is_regular_file(SourceSidecar))
		return;
	try
	{
		core::io::SecurePath::MoveWithin(SourceRoot, SourceSidecar.lexically_relative(SourceRoot), DestinationRoot,
										 AssetMetadataStore::GetSidecarPath(Destination).lexically_relative(DestinationRoot), false,
										 "Content metadata move");
	}
	catch (...)
	{
		core::io::SecurePath::MoveWithin(DestinationRoot, DestinationRelative, SourceRoot, SourceRelative, false,
										 "Content payload move rollback");
		throw;
	}
}

void EnsureDestinationAvailable(const std::filesystem::path &Destination)
{
	if (std::filesystem::exists(Destination) || std::filesystem::exists(AssetMetadataStore::GetSidecarPath(Destination)))
		throw AssetContentCollisionException("Content destination already exists: '" + Destination.string() + "'");
}

void RewriteMovedMetadata(const std::filesystem::path &ContentRoot, const std::filesystem::path &OldRelative,
						  const std::filesystem::path &NewRelative, const bool Directory,
						  const std::vector<MetadataRecord> &MetadataRecords)
{
	for (const MetadataRecord &Record : MetadataRecords)
	{
		const std::filesystem::path WithinSource =
			Directory ? Record.SourceRelativePath.lexically_relative(OldRelative) : std::filesystem::path{};
		const std::filesystem::path NewSourceRelative = Directory ? NewRelative / WithinSource : NewRelative;
		AssetMetadata Updated = Record.Metadata;
		Updated.VirtualSource = MakeVirtualPath(NewSourceRelative);
		Updated.PhysicalSourceIdentity = AssetMetadataStore::CalculatePhysicalSourceIdentity(ContentRoot / NewSourceRelative);
		Updated.SourceHash = AssetMetadataStore::CalculateSourceHash(ContentRoot / NewSourceRelative);
		AssetMetadataStore::Save(Updated, AssetMetadataStore::GetSidecarPath(ContentRoot / NewSourceRelative));
	}
}

void RestoreOriginalMetadata(const std::filesystem::path &ContentRoot, const std::vector<MetadataRecord> &MetadataRecords)
{
	for (const MetadataRecord &Record : MetadataRecords)
		AssetMetadataStore::Save(Record.Metadata, AssetMetadataStore::GetSidecarPath(ContentRoot / Record.SourceRelativePath));
}

void RewriteStringReferences(Json &Value, const std::unordered_map<string, string> &Remapped)
{
	if (Value.is_string())
	{
		const auto Found = Remapped.find(Value.get_ref<const string &>());
		if (Found != Remapped.end())
			Value = Found->second;
	}
	else if (Value.is_array())
	{
		for (Json &Child : Value)
			RewriteStringReferences(Child, Remapped);
	}
	else if (Value.is_object())
	{
		for (auto &[Name, Child] : Value.items())
		{
			(void)Name;
			RewriteStringReferences(Child, Remapped);
		}
	}
}

void RewriteSerializedAssetReferences(const std::filesystem::path &StagedTarget,
									  const std::unordered_map<resource::AssetID, resource::AssetID> &AssetIDs)
{
	std::vector<std::filesystem::path> Files;
	if (std::filesystem::is_regular_file(StagedTarget))
		Files.push_back(StagedTarget);
	else
	{
		for (const std::filesystem::directory_entry &Entry : std::filesystem::recursive_directory_iterator(StagedTarget))
		{
			if (Entry.is_regular_file() && !IsMetadataPath(Entry.path()))
				Files.push_back(Entry.path());
		}
	}
	for (const std::filesystem::path &File : Files)
	{
		const string Extension = File.extension().string();
		if (Extension != ".enginelevel" && Extension != ".material" && Extension != ".materialinstance")
			continue;
		Json Root = ReadJsonWithin(File.parent_path(), File.filename(), "Duplicated serialized asset");
		std::unordered_map<string, string> References(AssetIDs.begin(), AssetIDs.end());
		if (Extension == ".enginelevel" && Root.contains("Objects") && Root.at("Objects").is_array())
		{
			for (const Json &Object : Root.at("Objects"))
			{
				if (Object.contains("ID") && Object.at("ID").is_string())
					References.emplace(Object.at("ID").get<string>(), util::UUID::GenerateRandomUUID().ToString());
			}
			if (Root.contains("ID") && Root.at("ID").is_string())
				References.emplace(Root.at("ID").get<string>(), util::UUID::GenerateRandomUUID().ToString());
		}
		RewriteStringReferences(Root, References);
		const std::filesystem::path Temporary = File.filename().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
		const string Serialized = Root.dump(2) + '\n';
		core::io::SecurePath::WriteFileWithin(File.parent_path(), Temporary,
											  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), false, true,
											  "Duplicated serialized reference rewrite");
		core::io::SecurePath::ReplaceWithin(File.parent_path(), Temporary, File.filename(), "Duplicated serialized reference publication");
	}
}

void RegenerateCopiedMetadata(const std::filesystem::path &StagedTarget, const std::filesystem::path &DestinationRelative)
{
	const std::filesystem::path StageParent = StagedTarget.parent_path();
	std::vector<MetadataRecord> Records = ReadMetadataTree(StagedTarget, StageParent);
	std::unordered_map<resource::AssetID, resource::AssetID> RemappedIDs;
	for (MetadataRecord &Record : Records)
		RemappedIDs.emplace(Record.Metadata.ID, util::UUID::GenerateRandomUUID().ToString());
	for (MetadataRecord &Record : Records)
	{
		const std::filesystem::path StagedSource = StageParent / Record.SourceRelativePath;
		const std::filesystem::path WithinTarget = Record.SourceRelativePath.lexically_relative(StagedTarget.filename());
		const std::filesystem::path FinalSource =
			std::filesystem::is_directory(StagedTarget) ? DestinationRelative / WithinTarget : DestinationRelative;
		Record.Metadata.ID = RemappedIDs.at(Record.Metadata.ID);
		for (resource::AssetID &Dependency : Record.Metadata.Dependencies)
		{
			const auto Remapped = RemappedIDs.find(Dependency);
			if (Remapped != RemappedIDs.end())
				Dependency = Remapped->second;
		}
		for (DerivedAssetMetadata &Derived : Record.Metadata.DerivedAssets)
			Derived.ID = util::UUID::GenerateRandomUUID().ToString();
		Record.Metadata.VirtualSource = MakeVirtualPath(FinalSource);
		Record.Metadata.PhysicalSourceIdentity = AssetMetadataStore::CalculatePhysicalSourceIdentity(StagedSource);
		Record.Metadata.SourceHash = AssetMetadataStore::CalculateSourceHash(StagedSource);
		AssetMetadataStore::Save(Record.Metadata, AssetMetadataStore::GetSidecarPath(StagedSource));
	}
	RewriteSerializedAssetReferences(StagedTarget, RemappedIDs);
	std::unordered_set<resource::AssetID> PublishedIDs;
	for (const MetadataRecord &Record : Records)
	{
		if (!PublishedIDs.emplace(Record.Metadata.ID).second)
			throw AssetContentTransactionException("Duplicated asset graph contains a duplicate remapped identity");
		for (const resource::AssetID &Dependency : Record.Metadata.Dependencies)
		{
			if (RemappedIDs.contains(Dependency))
				throw AssetContentTransactionException("Duplicated asset graph retained an obsolete internal dependency");
		}
	}
}

void PublishManifest(const TrashedContentEntry &Entry, const std::filesystem::path &ManifestPath)
{
	const Json Root{{"FormatVersion", 1U},
					{"ID", Entry.ID.ToString()},
					{"OriginalPath", Entry.OriginalPath.generic_string()},
					{"StoredPath", Entry.StoredPath.generic_string()},
					{"Directory", Entry.Directory},
					{"TimestampMilliseconds", Entry.TimestampMilliseconds}};
	const std::filesystem::path Temporary = ManifestPath.filename().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	const string Serialized = Root.dump(2) + '\n';
	core::io::SecurePath::WriteFileWithin(ManifestPath.parent_path(), Temporary,
										  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), false, true,
										  "Trash manifest");
	core::io::SecurePath::ReplaceWithin(ManifestPath.parent_path(), Temporary, ManifestPath.filename(), "Trash manifest publication");
}

[[nodiscard]] std::filesystem::path OperationJournalPath(const std::filesystem::path &IntermediateRoot, const util::UUID &OperationID)
{
	return IntermediateRoot / "ContentOperations" / (OperationID.ToString() + ".journal.json");
}

void WriteOperationJournal(const std::filesystem::path &ContentRoot, const std::filesystem::path &IntermediateRoot,
						   const util::UUID &OperationID, const AssetContentRequest &Request, const string_view Phase)
{
	const std::filesystem::path Path = OperationJournalPath(IntermediateRoot, OperationID);
	Json Root;
	if (Phase == "FilesystemCommitted" && std::filesystem::is_regular_file(Path))
	{
		Root = ReadJsonWithin(IntermediateRoot, Path.lexically_relative(IntermediateRoot), "Content-operation journal");
		Root["Phase"] = Phase;
	}
	else
	{
		string SourceIdentity;
		bool SourceExisted = false;
		bool DestinationExisted = false;
		if (!Request.Source.empty())
		{
			const std::filesystem::path Source = ResolveWithin(ContentRoot, Request.Source, "Journal source");
			SourceExisted = std::filesystem::exists(Source);
			if (SourceExisted)
				SourceIdentity = AssetMetadataStore::CalculatePhysicalSourceIdentity(Source);
		}
		if (!Request.Destination.empty())
		{
			const std::filesystem::path Destination = ResolveWithin(ContentRoot, Request.Destination, "Journal destination");
			DestinationExisted = std::filesystem::exists(Destination);
		}
		Root = {{"FormatVersion", 2U},
				{"OperationID", OperationID.ToString()},
				{"Operation", static_cast<uint32>(Request.Operation)},
				{"Source", Request.Source.generic_string()},
				{"Destination", Request.Destination.generic_string()},
				{"TrashEntryID", Request.TrashEntryID.IsValid() ? Request.TrashEntryID.ToString() : string{}},
				{"SourceIdentity", SourceIdentity},
				{"SourceExisted", SourceExisted},
				{"DestinationExisted", DestinationExisted},
				{"Phase", Phase}};
	}
	core::io::SecurePath::CreateDirectoriesWithin(IntermediateRoot, "ContentOperations", "Content-operation journal root");
	const std::filesystem::path RelativePath = Path.lexically_relative(IntermediateRoot);
	const std::filesystem::path Temporary =
		RelativePath.parent_path() / (RelativePath.filename().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString());
	const string Serialized = Root.dump() + '\n';
	core::io::SecurePath::WriteFileWithin(IntermediateRoot, Temporary,
										  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), false, true,
										  "Content-operation journal");
	core::io::SecurePath::ReplaceWithin(IntermediateRoot, Temporary, RelativePath, "Content-operation journal publication");
}

[[nodiscard]] bool RecoverOperationJournals(const std::filesystem::path &ContentRoot, const std::filesystem::path &IntermediateRoot,
											const std::filesystem::path &TrashRoot, core::threading::TaskScheduler &Scheduler,
											AssetRegistry &Registry)
{
	const std::filesystem::path Root = IntermediateRoot / "ContentOperations";
	std::vector<std::filesystem::path> Journals;
	std::error_code Error;
	for (std::filesystem::directory_iterator Iterator(Root, std::filesystem::directory_options::skip_permission_denied, Error), End;
		 !Error && Iterator != End; Iterator.increment(Error))
	{
		if (Iterator->is_regular_file() && Iterator->path().extension() == ".json" &&
			Iterator->path().filename().string().ends_with(".journal.json"))
			Journals.push_back(Iterator->path());
	}
	if (Error)
		throw AssetContentTransactionException("Could not enumerate content-operation recovery journals: " + Error.message());
	if (Journals.empty())
		return false;
	bool RegistryChanged = false;
	for (const std::filesystem::path &Journal : Journals)
	{
		const Json Entry =
			ReadJsonWithin(IntermediateRoot, Journal.lexically_relative(IntermediateRoot), "Content-operation recovery journal");
		const uint32 FormatVersion = Entry.value("FormatVersion", 0U);
		if ((FormatVersion != 1U && FormatVersion != 2U) || !util::UUID::TryParse(Entry.value("OperationID", string{})).has_value())
			throw AssetContentTransactionException("Content-operation recovery journal is invalid");
		const util::UUID OperationID = util::UUID::Parse(Entry.at("OperationID").get<string>());
		const auto Operation = static_cast<AssetContentOperation>(Entry.at("Operation").get<uint32>());
		const std::filesystem::path SourceRelative = Entry.value("Source", string{});
		const std::filesystem::path DestinationRelative = Entry.value("Destination", string{});
		const string Phase = Entry.value("Phase", string{});
		if (static_cast<uint32>(Operation) > static_cast<uint32>(AssetContentOperation::Restore) ||
			(Phase != "Started" && Phase != "FilesystemCommitted"))
			throw AssetContentTransactionException("Content-operation recovery journal contract is invalid");
		bool Committed = Phase == "FilesystemCommitted";
		const string ExpectedSourceIdentity = Entry.value("SourceIdentity", string{});
		if (!ExpectedSourceIdentity.empty() && !SourceRelative.empty())
		{
			const std::filesystem::path Source = ResolveWithin(ContentRoot, SourceRelative, "Recovery source identity");
			if (std::filesystem::exists(Source) && AssetMetadataStore::CalculatePhysicalSourceIdentity(Source) != ExpectedSourceIdentity)
				throw AssetContentTransactionException("Interrupted content source identity no longer matches its journal");
		}
		if (!Committed)
		{
			switch (Operation)
			{
			case AssetContentOperation::CreateFolder:
			case AssetContentOperation::CreateMaterial:
			case AssetContentOperation::CreateMaterialInstance:
				Committed = std::filesystem::exists(ResolveWithin(ContentRoot, DestinationRelative, "Recovery destination"));
				break;
			case AssetContentOperation::Move:
			{
				const bool SourceExists = std::filesystem::exists(ResolveWithin(ContentRoot, SourceRelative, "Recovery source"));
				const bool DestinationExists =
					std::filesystem::exists(ResolveWithin(ContentRoot, DestinationRelative, "Recovery destination"));
				const bool SourceExisted = Entry.value("SourceExisted", true);
				const bool DestinationExisted = Entry.value("DestinationExisted", false);
				if (SourceExists == SourceExisted && DestinationExists == DestinationExisted)
				{
					Committed = false;
					break;
				}
				if (!SourceExisted || DestinationExisted || SourceExists || !DestinationExists)
					throw AssetContentTransactionException("Interrupted move has an ambiguous filesystem state");
				Committed = true;
				break;
			}
			case AssetContentOperation::Duplicate:
				Committed = std::filesystem::exists(ResolveWithin(ContentRoot, DestinationRelative, "Recovery destination"));
				break;
			case AssetContentOperation::Trash:
			{
				const std::filesystem::path Source = ResolveWithin(ContentRoot, SourceRelative, "Recovery source");
				const std::filesystem::path EntryRoot = TrashRoot / OperationID.ToString();
				const std::filesystem::path Stored = EntryRoot / "Payload" / Source.filename();
				const bool SourceExists = std::filesystem::exists(Source);
				const bool StoredExists = std::filesystem::exists(Stored);
				if (SourceExists == StoredExists)
					throw AssetContentTransactionException("Interrupted trash operation has an ambiguous filesystem state");
				Committed = StoredExists;
				if (Committed && !std::filesystem::is_regular_file(EntryRoot / "Trash.json"))
				{
					PublishManifest({.ID = OperationID,
									 .OriginalPath = SourceRelative,
									 .StoredPath = Stored.lexically_relative(TrashRoot),
									 .Directory = std::filesystem::is_directory(Stored),
									 .TimestampMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
																  std::chrono::system_clock::now().time_since_epoch())
																  .count()},
									EntryRoot / "Trash.json");
				}
				break;
			}
			case AssetContentOperation::Restore:
			{
				const util::UUID TrashID = util::UUID::Parse(Entry.at("TrashEntryID").get<string>());
				const std::filesystem::path EntryRoot = TrashRoot / TrashID.ToString();
				if (!std::filesystem::is_regular_file(EntryRoot / "Trash.json"))
				{
					Committed = true;
					break;
				}
				const Json Manifest =
					ReadJsonWithin(TrashRoot, (EntryRoot / "Trash.json").lexically_relative(TrashRoot), "Content trash recovery manifest");
				const std::filesystem::path Stored =
					ResolveWithin(TrashRoot, Manifest.at("StoredPath").get<string>(), "Recovery stored path");
				const std::filesystem::path Destination =
					ResolveWithin(ContentRoot, Manifest.at("OriginalPath").get<string>(), "Recovery destination");
				const bool StoredExists = std::filesystem::exists(Stored);
				const bool DestinationExists = std::filesystem::exists(Destination);
				if (StoredExists == DestinationExists)
					throw AssetContentTransactionException("Interrupted restore has an ambiguous filesystem state");
				Committed = DestinationExists;
				break;
			}
			}
		}
		RegistryChanged |= Committed;
		const std::filesystem::path StagingRelative = std::filesystem::path("ContentOperations") / OperationID.ToString();
		if (std::filesystem::exists(IntermediateRoot / StagingRelative))
			core::io::SecurePath::RemoveWithin(IntermediateRoot, StagingRelative, true, "Interrupted content staging");
	}
	if (RegistryChanged)
		Registry.RequestRefresh(Scheduler, true);
	for (const std::filesystem::path &Journal : Journals)
	{
		core::io::SecurePath::RemoveWithin(IntermediateRoot, Journal.lexically_relative(IntermediateRoot), false,
										   "Recovered content-operation journal");
	}
	return true;
}

[[nodiscard]] TrashedContentEntry ReadTrashManifest(const std::filesystem::path &Path)
{
	const Json Root = ReadJsonWithin(Path.parent_path(), Path.filename(), "Trash manifest");
	if (!Root.is_object() || Root.value("FormatVersion", 0U) != 1U)
		throw AssetContentTransactionException("Trash manifest has an unsupported format");
	TrashedContentEntry Entry{.ID = util::UUID::Parse(Root.at("ID").get<string>()),
							  .OriginalPath = Root.at("OriginalPath").get<string>(),
							  .StoredPath = Root.at("StoredPath").get<string>(),
							  .Directory = Root.at("Directory").get<bool>(),
							  .TimestampMilliseconds = Root.at("TimestampMilliseconds").get<int64>()};
	(void)NormalizeRelative(Entry.OriginalPath, "Trash original path");
	(void)NormalizeRelative(Entry.StoredPath, "Trash stored path");
	return Entry;
}

[[nodiscard]] AssetContentResult ExecuteCreate(const std::filesystem::path &ContentRoot, AssetContentRequest Request,
											   const util::UUID &OperationID,
											   const std::shared_ptr<AssetContentService::SharedOperationState> &Operation)
{
	const std::filesystem::path DestinationRelative = NormalizeRelative(Request.Destination, "Content destination");
	const std::filesystem::path Destination = ResolveWithin(ContentRoot, DestinationRelative, "Content destination");
	EnsureDestinationAvailable(Destination);
	if (Operation->CancelRequested.load(std::memory_order_acquire))
	{
		return {.OperationID = OperationID,
				.Operation = Request.Operation,
				.Destination = DestinationRelative,
				.Diagnostic = "Content creation cancelled before commit"};
	}

	if (Request.Operation == AssetContentOperation::CreateFolder)
	{
		core::io::SecurePath::CreateDirectoriesWithin(ContentRoot, DestinationRelative, "Content directory creation");
	}
	else
	{
		material::MaterialDocument Document{.DocumentID = util::UUID::GenerateRandomUUID().ToString(),
											.Type = Request.Operation == AssetContentOperation::CreateMaterial
														? material::MaterialDocumentType::Material
														: material::MaterialDocumentType::MaterialInstance,
											.Name = Destination.stem().string(),
											.Path = Destination};
		if (Document.Type == material::MaterialDocumentType::MaterialInstance)
			Document.Parent = material::MaterialParentReference{.ID = Request.ParentAssetID, .Type = Request.ParentAssetType};
		material::MaterialDocumentStore::Save(Document, Destination);
	}

	return {.OperationID = OperationID,
			.Operation = Request.Operation,
			.Destination = DestinationRelative,
			.Committed = true,
			.Diagnostic = Request.Operation == AssetContentOperation::CreateFolder ? "Content directory created successfully"
																				   : "Material asset created successfully"};
}

[[nodiscard]] AssetContentResult ExecuteMove(const std::filesystem::path &ContentRoot, AssetContentRequest Request,
											 const util::UUID &OperationID,
											 const std::shared_ptr<AssetContentService::SharedOperationState> &Operation)
{
	const std::filesystem::path SourceRelative = NormalizeRelative(Request.Source, "Content source");
	const std::filesystem::path DestinationRelative = NormalizeRelative(Request.Destination, "Content destination");
	const std::filesystem::path Source = ResolveWithin(ContentRoot, SourceRelative, "Content source");
	const std::filesystem::path Destination = ResolveWithin(ContentRoot, DestinationRelative, "Content destination");
	if (!std::filesystem::exists(Source))
		throw AssetContentNotFoundException("Content source does not exist: '" + Source.string() + "'");
	EnsureDestinationAvailable(Destination);
	if (Operation->CancelRequested.load(std::memory_order_acquire))
		return {.OperationID = OperationID,
				.Operation = Request.Operation,
				.Source = SourceRelative,
				.Destination = DestinationRelative,
				.Diagnostic = "Content operation cancelled before commit"};
	const bool Directory = std::filesystem::is_directory(Source);
	const std::vector<MetadataRecord> Records = ReadMetadataTree(Source, ContentRoot);
	RenamePayload(ContentRoot, SourceRelative, ContentRoot, DestinationRelative);
	try
	{
		RewriteMovedMetadata(ContentRoot, SourceRelative, DestinationRelative, Directory, Records);
	}
	catch (...)
	{
		try
		{
			RenamePayload(ContentRoot, DestinationRelative, ContentRoot, SourceRelative);
			RestoreOriginalMetadata(ContentRoot, Records);
		}
		catch (...)
		{
			throw AssetContentTransactionException("Content move failed and rollback could not restore the original state");
		}
		throw;
	}
	return {.OperationID = OperationID,
			.Operation = Request.Operation,
			.Source = SourceRelative,
			.Destination = DestinationRelative,
			.Committed = true,
			.Diagnostic = "Content moved successfully"};
}

[[nodiscard]] AssetContentResult ExecuteDuplicate(const std::filesystem::path &ContentRoot, const std::filesystem::path &IntermediateRoot,
												  AssetContentRequest Request, const util::UUID &OperationID,
												  const std::shared_ptr<AssetContentService::SharedOperationState> &Operation)
{
	const std::filesystem::path SourceRelative = NormalizeRelative(Request.Source, "Content source");
	const std::filesystem::path DestinationRelative = NormalizeRelative(Request.Destination, "Content destination");
	const std::filesystem::path Source = ResolveWithin(ContentRoot, SourceRelative, "Content source");
	const std::filesystem::path Destination = ResolveWithin(ContentRoot, DestinationRelative, "Content destination");
	if (!std::filesystem::exists(Source))
		throw AssetContentNotFoundException("Content source does not exist: '" + Source.string() + "'");
	EnsureDestinationAvailable(Destination);
	const std::filesystem::path StageRelative = std::filesystem::path("ContentOperations") / OperationID.ToString();
	const std::filesystem::path StageRoot = IntermediateRoot / StageRelative;
	core::io::SecurePath::CreateDirectoriesWithin(IntermediateRoot, StageRelative / "Payload", "Content duplicate staging");
	StagingScope Stage(IntermediateRoot, StageRelative);
	const std::filesystem::path StagedTarget = Stage.GetPath() / "Payload" / Destination.filename();
	CopyPayload(ContentRoot, SourceRelative, IntermediateRoot, StagedTarget.lexically_relative(IntermediateRoot));
	if (Operation->CancelRequested.load(std::memory_order_acquire))
		return {.OperationID = OperationID,
				.Operation = Request.Operation,
				.Source = SourceRelative,
				.Destination = DestinationRelative,
				.Diagnostic = "Content duplication cancelled before commit"};
	RegenerateCopiedMetadata(StagedTarget, DestinationRelative);
	RenamePayload(IntermediateRoot, StagedTarget.lexically_relative(IntermediateRoot), ContentRoot, DestinationRelative);
	return {.OperationID = OperationID,
			.Operation = Request.Operation,
			.Source = SourceRelative,
			.Destination = DestinationRelative,
			.Committed = true,
			.Diagnostic = "Content duplicated with new stable asset identities"};
}

[[nodiscard]] AssetContentResult ExecuteTrash(const std::filesystem::path &ContentRoot, const std::filesystem::path &TrashRoot,
											  AssetContentRequest Request, const util::UUID &OperationID,
											  const std::shared_ptr<AssetContentService::SharedOperationState> &Operation)
{
	const std::filesystem::path SourceRelative = NormalizeRelative(Request.Source, "Content source");
	const std::filesystem::path Source = ResolveWithin(ContentRoot, SourceRelative, "Content source");
	if (!std::filesystem::exists(Source))
		throw AssetContentNotFoundException("Content source does not exist: '" + Source.string() + "'");
	if (Operation->CancelRequested.load(std::memory_order_acquire))
		return {.OperationID = OperationID,
				.Operation = Request.Operation,
				.Source = SourceRelative,
				.Diagnostic = "Trash operation cancelled before commit"};
	const std::filesystem::path EntryRoot = TrashRoot / OperationID.ToString();
	const std::filesystem::path StoredRelative = std::filesystem::path(OperationID.ToString()) / "Payload" / Source.filename();
	const std::filesystem::path Stored = TrashRoot / StoredRelative;
	core::io::SecurePath::CreateDirectoriesWithin(TrashRoot, StoredRelative.parent_path(), "Trash payload staging");
	RenamePayload(ContentRoot, SourceRelative, TrashRoot, StoredRelative);
	const TrashedContentEntry Entry{
		.ID = OperationID,
		.OriginalPath = SourceRelative,
		.StoredPath = StoredRelative,
		.Directory = std::filesystem::is_directory(Stored),
		.TimestampMilliseconds =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
	try
	{
		PublishManifest(Entry, EntryRoot / "Trash.json");
	}
	catch (...)
	{
		try
		{
			RenamePayload(TrashRoot, StoredRelative, ContentRoot, SourceRelative);
			core::io::SecurePath::RemoveWithin(TrashRoot, EntryRoot.lexically_relative(TrashRoot), true, "Trash rollback");
		}
		catch (...)
		{
			throw AssetContentTransactionException("Trash publication failed and rollback could not restore the source");
		}
		throw;
	}
	return {.OperationID = OperationID,
			.Operation = Request.Operation,
			.Source = SourceRelative,
			.Committed = true,
			.Diagnostic = "Content moved to project trash"};
}

[[nodiscard]] AssetContentResult ExecuteRestore(const std::filesystem::path &ContentRoot, const std::filesystem::path &TrashRoot,
												AssetContentRequest Request, const util::UUID &OperationID,
												const std::shared_ptr<AssetContentService::SharedOperationState> &Operation)
{
	if (!Request.TrashEntryID.IsValid())
		throw std::invalid_argument("Restore requires a valid trash entry identity");
	const std::filesystem::path EntryRoot = TrashRoot / Request.TrashEntryID.ToString();
	const TrashedContentEntry Entry = ReadTrashManifest(EntryRoot / "Trash.json");
	if (Entry.ID != Request.TrashEntryID)
		throw AssetContentTransactionException("Trash manifest identity does not match the requested entry");
	const std::filesystem::path Stored = ResolveWithin(TrashRoot, Entry.StoredPath, "Trash stored path");
	const std::filesystem::path Destination = ResolveWithin(ContentRoot, Entry.OriginalPath, "Trash original path");
	if (!std::filesystem::exists(Stored))
		throw AssetContentNotFoundException("Trashed content payload is missing");
	EnsureDestinationAvailable(Destination);
	if (Operation->CancelRequested.load(std::memory_order_acquire))
		return {.OperationID = OperationID,
				.Operation = Request.Operation,
				.Destination = Entry.OriginalPath,
				.Diagnostic = "Restore operation cancelled before commit"};
	RenamePayload(TrashRoot, Entry.StoredPath, ContentRoot, Entry.OriginalPath);
	string CleanupDiagnostic;
	try
	{
		core::io::SecurePath::RemoveWithin(TrashRoot, EntryRoot.lexically_relative(TrashRoot), true, "Trash restore cleanup");
	}
	catch (const std::exception &Exception)
	{
		CleanupDiagnostic = Exception.what();
	}
	return {.OperationID = OperationID,
			.Operation = Request.Operation,
			.Source = Entry.StoredPath,
			.Destination = Entry.OriginalPath,
			.Committed = true,
			.Diagnostic = CleanupDiagnostic.empty()
							  ? "Content restored successfully"
							  : "Content restored; trash bookkeeping cleanup requires attention: " + CleanupDiagnostic};
}
} // namespace

AssetContentService::AssetContentService(std::filesystem::path ContentRoot, std::filesystem::path IntermediateRoot,
										 std::filesystem::path TrashRoot)
	: ContentRoot(std::filesystem::absolute(std::move(ContentRoot)).lexically_normal()),
	  IntermediateRoot(std::filesystem::absolute(std::move(IntermediateRoot)).lexically_normal()),
	  TrashRoot(std::filesystem::absolute(std::move(TrashRoot)).lexically_normal()), OwnerThread(std::this_thread::get_id())
{
	if (this->ContentRoot.empty() || this->IntermediateRoot.empty() || this->TrashRoot.empty())
		throw std::invalid_argument("Asset content service requires explicit storage roots");
	core::io::SecurePath::VerifyContained(this->ContentRoot, this->ContentRoot, "Content service root");
	core::io::SecurePath::CreateTrustedRoot(this->IntermediateRoot, "Intermediate service root");
	core::io::SecurePath::CreateDirectoriesWithin(this->IntermediateRoot, "ContentOperations", "Content operation storage");
	core::io::SecurePath::CreateTrustedRoot(this->TrashRoot, "Trash service root");
}

AssetContentService::~AssetContentService()
{
	this->Cancel();
	this->Wait();
}

void AssetContentService::Queue(AssetContentRequest Request, core::threading::TaskScheduler &Scheduler)
{
	this->VerifyOwnerThread();
	if (this->IsBusy())
		throw AssetContentException("A content operation is already active");
	if (Request.Operation == AssetContentOperation::Restore)
	{
		if (!Request.TrashEntryID.IsValid())
			throw std::invalid_argument("Restore requires a valid trash entry identity");
	}
	else
	{
		if (Request.Operation == AssetContentOperation::Move || Request.Operation == AssetContentOperation::Duplicate ||
			Request.Operation == AssetContentOperation::Trash)
		{
			(void)NormalizeRelative(Request.Source, "Content source");
		}
		if (Request.Operation == AssetContentOperation::Move || Request.Operation == AssetContentOperation::Duplicate ||
			Request.Operation == AssetContentOperation::CreateFolder || Request.Operation == AssetContentOperation::CreateMaterial ||
			Request.Operation == AssetContentOperation::CreateMaterialInstance)
		{
			(void)NormalizeRelative(Request.Destination, "Content destination");
		}
		if (Request.Operation == AssetContentOperation::CreateMaterialInstance)
		{
			if (Request.ParentAssetID.empty())
				throw std::invalid_argument("Material instance creation requires a valid parent asset identity");
			if (Request.ParentAssetType != resource::AssetType::Material &&
				Request.ParentAssetType != resource::AssetType::MaterialInstance)
			{
				throw std::invalid_argument("Material instance parent must be a material or material instance");
			}
		}
	}
	this->Result.reset();
	this->Operation = std::make_shared<SharedOperationState>();
	const std::filesystem::path Content = this->ContentRoot;
	const std::filesystem::path Intermediate = this->IntermediateRoot;
	const std::filesystem::path Trash = this->TrashRoot;
	const std::shared_ptr<SharedOperationState> SharedOperation = this->Operation;
	this->Pending =
		Scheduler.Submit([Content, Intermediate, Trash, Request = std::move(Request), SharedOperation]() mutable
						 { return AssetContentService::Execute(Content, Intermediate, Trash, std::move(Request), SharedOperation); },
						 core::threading::TaskPriority::Background);
	this->State = AssetContentServiceState::Running;
}

bool AssetContentService::Poll(core::threading::TaskScheduler &Scheduler, AssetRegistry &Registry)
{
	this->VerifyOwnerThread();
	if (this->State != AssetContentServiceState::Running)
		return RecoverOperationJournals(this->ContentRoot, this->IntermediateRoot, this->TrashRoot, Scheduler, Registry);
	if (this->State != AssetContentServiceState::Running || !this->Pending.valid() ||
		this->Pending.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
	{
		return false;
	}
	try
	{
		this->Result = this->Pending.get();
		this->State = this->Result->Committed
						  ? AssetContentServiceState::Completed
						  : (this->Operation->CancelRequested.load(std::memory_order_acquire) ? AssetContentServiceState::Cancelled
																							  : AssetContentServiceState::Failed);
		if (this->Result->Committed)
		{
			Registry.RequestRefresh(Scheduler, true);
			core::io::SecurePath::RemoveWithin(
				this->IntermediateRoot,
				OperationJournalPath(this->IntermediateRoot, this->Result->OperationID).lexically_relative(this->IntermediateRoot), false,
				"Committed content-operation journal");
		}
	}
	catch (const std::exception &Exception)
	{
		this->Result = AssetContentResult{.OperationID = util::UUID::GenerateRandomUUID(), .Diagnostic = Exception.what()};
		this->State = AssetContentServiceState::Failed;
	}
	catch (...)
	{
		this->Result = AssetContentResult{.OperationID = util::UUID::GenerateRandomUUID(),
										  .Diagnostic = "Content operation failed with a non-standard exception"};
		this->State = AssetContentServiceState::Failed;
	}
	return true;
}

void AssetContentService::Cancel() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	if (this->Operation != nullptr)
		this->Operation->CancelRequested.store(true, std::memory_order_release);
}

void AssetContentService::Wait() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	if (!this->Pending.valid())
		return;
	try
	{
		this->Result = this->Pending.get();
		this->State = this->Result->Committed
						  ? AssetContentServiceState::Completed
						  : (this->Operation != nullptr && this->Operation->CancelRequested.load(std::memory_order_acquire)
								 ? AssetContentServiceState::Cancelled
								 : AssetContentServiceState::Failed);
	}
	catch (const std::exception &Exception)
	{
		this->Result = AssetContentResult{.OperationID = util::UUID::GenerateRandomUUID(), .Diagnostic = Exception.what()};
		this->State = AssetContentServiceState::Failed;
	}
	catch (...)
	{
		this->Result = AssetContentResult{.OperationID = util::UUID::GenerateRandomUUID(),
										  .Diagnostic = "Content operation failed with a non-standard exception"};
		this->State = AssetContentServiceState::Failed;
	}
}

void AssetContentService::Reset()
{
	this->VerifyOwnerThread();
	if (this->IsBusy())
		throw AssetContentException("Cannot reset an active content operation");
	this->Result.reset();
	this->Operation.reset();
	this->State = AssetContentServiceState::Idle;
}

std::vector<TrashedContentEntry> AssetContentService::ScanTrash() const
{
	this->VerifyOwnerThread();
	std::vector<TrashedContentEntry> Result;
	std::error_code Error;
	for (std::filesystem::directory_iterator Iterator(this->TrashRoot, std::filesystem::directory_options::skip_permission_denied, Error),
		 End;
		 !Error && Iterator != End; Iterator.increment(Error))
	{
		if (!Iterator->is_directory(Error) || Error)
			continue;
		const std::filesystem::path Manifest = Iterator->path() / "Trash.json";
		if (!std::filesystem::is_regular_file(Manifest, Error) || Error)
			continue;
		Result.push_back(ReadTrashManifest(Manifest));
	}
	if (Error)
		throw AssetContentTransactionException("Could not enumerate project trash: " + Error.message());
	std::ranges::sort(Result, std::greater{}, &TrashedContentEntry::TimestampMilliseconds);
	return Result;
}

AssetContentServiceState AssetContentService::GetState() const noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	return this->State;
}

bool AssetContentService::IsBusy() const noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	return this->State == AssetContentServiceState::Running;
}

std::optional<AssetContentResult> AssetContentService::GetResult() const
{
	this->VerifyOwnerThread();
	return this->Result;
}

void AssetContentService::VerifyOwnerThread() const
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw AssetContentException("Asset content service must be accessed from its owner thread");
}

AssetContentResult AssetContentService::Execute(const std::filesystem::path &ContentRoot, const std::filesystem::path &IntermediateRoot,
												const std::filesystem::path &TrashRoot, AssetContentRequest Request,
												const std::shared_ptr<SharedOperationState> &Operation)
{
	if (Operation == nullptr)
		throw std::invalid_argument("Content operation requires shared cancellation state");
	const util::UUID OperationID = util::UUID::GenerateRandomUUID();
	WriteOperationJournal(ContentRoot, IntermediateRoot, OperationID, Request, "Started");
	AssetContentResult Result;
	switch (Request.Operation)
	{
	case AssetContentOperation::CreateFolder:
	case AssetContentOperation::CreateMaterial:
	case AssetContentOperation::CreateMaterialInstance:
		Result = ExecuteCreate(ContentRoot, Request, OperationID, Operation);
		break;
	case AssetContentOperation::Move:
		Result = ExecuteMove(ContentRoot, Request, OperationID, Operation);
		break;
	case AssetContentOperation::Duplicate:
		Result = ExecuteDuplicate(ContentRoot, IntermediateRoot, Request, OperationID, Operation);
		break;
	case AssetContentOperation::Trash:
		Result = ExecuteTrash(ContentRoot, TrashRoot, Request, OperationID, Operation);
		break;
	case AssetContentOperation::Restore:
		Result = ExecuteRestore(ContentRoot, TrashRoot, Request, OperationID, Operation);
		break;
	default:
		throw std::logic_error("Unknown asset content operation");
	}
	if (Result.Committed)
		WriteOperationJournal(ContentRoot, IntermediateRoot, OperationID, Request, "FilesystemCommitted");
	else
	{
		const std::filesystem::path Journal = OperationJournalPath(IntermediateRoot, OperationID);
		if (std::filesystem::exists(Journal))
			core::io::SecurePath::RemoveWithin(IntermediateRoot, Journal.lexically_relative(IntermediateRoot), false,
											   "Cancelled content-operation journal");
	}
	return Result;
}
} // namespace editor::asset
