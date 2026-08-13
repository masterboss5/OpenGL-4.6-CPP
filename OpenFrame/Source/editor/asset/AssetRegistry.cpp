#include "AssetRegistry.h"

#include "Source/core/io/SecurePath.h"
#include "Source/resource/asset/AssetManager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <span>
#include <system_error>
#include <unordered_map>

namespace editor::asset
{
namespace
{
[[nodiscard]] string Lowercase(string Text)
{
	std::ranges::transform(Text, Text.begin(),
						   [](const char Character) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
	return Text;
}

[[nodiscard]] util::UUID HashPathIdentity(const std::filesystem::path &RelativePath)
{
	const string Path = Lowercase(RelativePath.generic_string());
	uint64 Left = 14'695'981'039'346'656'037ULL;
	uint64 Right = 1'099'511'628'211ULL;
	for (const uint8 Byte : std::span(reinterpret_cast<const uint8 *>(Path.data()), Path.size()))
	{
		Left = (Left ^ Byte) * 1'099'511'628'211ULL;
		Right ^= static_cast<uint64>(Byte) + 0x9e3779b97f4a7c15ULL + (Right << 6U) + (Right >> 2U);
	}
	if (Left == 0 && Right == 0)
		Right = 1;
	return {Left, Right};
}

[[nodiscard]] util::UUID ReadFileIdentity(const std::filesystem::path &Path, const std::filesystem::path &RelativePath,
										  const bool Directory)
{
	const DWORD Flags = Directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
	const HANDLE File = CreateFileW(Path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
									OPEN_EXISTING, Flags, nullptr);
	if (File == INVALID_HANDLE_VALUE)
		return HashPathIdentity(RelativePath);
	FILE_ID_INFO Identity{};
	if (GetFileInformationByHandleEx(File, FileIdInfo, &Identity, sizeof(Identity)) == FALSE)
	{
		BY_HANDLE_FILE_INFORMATION LegacyIdentity{};
		if (GetFileInformationByHandle(File, &LegacyIdentity) != FALSE)
		{
			(void)CloseHandle(File);
			const uint64 Left =
				(static_cast<uint64>(LegacyIdentity.dwVolumeSerialNumber) << 32U) | static_cast<uint64>(LegacyIdentity.nFileIndexHigh);
			const uint64 Right = static_cast<uint64>(LegacyIdentity.nFileIndexLow);
			if (Left != 0 || Right != 0)
				return {Left, Right};
			return HashPathIdentity(RelativePath);
		}
		(void)CloseHandle(File);
		return HashPathIdentity(RelativePath);
	}
	(void)CloseHandle(File);
	uint64 Left = Identity.VolumeSerialNumber;
	uint64 Right = 0;
	std::memcpy(&Right, Identity.FileId.Identifier, sizeof(Right));
	uint64 Tail = 0;
	std::memcpy(&Tail, Identity.FileId.Identifier + sizeof(Right), sizeof(Tail));
	Left ^= Tail + 0x9e3779b97f4a7c15ULL + (Left << 6U) + (Left >> 2U);
	if (Left == 0 && Right == 0)
		return HashPathIdentity(RelativePath);
	return {Left, Right};
}

struct ClassifiedEntry final
{
	ContentEntryKind Kind = ContentEntryKind::Unknown;
	std::optional<resource::AssetType> Type;
};

[[nodiscard]] string AssetTypeName(const ClassifiedEntry &Classification)
{
	if (Classification.Kind == ContentEntryKind::Scene)
		return "Scene";
	if (Classification.Kind == ContentEntryKind::SourceFile)
		return "SourceFile";
	if (!Classification.Type.has_value())
		return "Unknown";
	switch (*Classification.Type)
	{
	case resource::AssetType::Texture2D:
		return "Texture2D";
	case resource::AssetType::Material:
		return "Material";
	case resource::AssetType::MaterialInstance:
		return "MaterialInstance";
	case resource::AssetType::Model:
		return "Model";
	case resource::AssetType::StaticMesh:
		return "StaticMesh";
	case resource::AssetType::SkeletalMesh:
		return "SkeletalMesh";
	case resource::AssetType::Skeleton:
		return "Skeleton";
	case resource::AssetType::AnimationClip:
		return "AnimationClip";
	case resource::AssetType::AnimationGraph:
		return "AnimationGraph";
	case resource::AssetType::RetargetProfile:
		return "RetargetProfile";
	case resource::AssetType::ShaderSource:
		return "ShaderSource";
	case resource::AssetType::Count:
		break;
	}
	return "Unknown";
}

[[nodiscard]] ClassifiedEntry Classify(const bool Directory, const string_view Extension)
{
	if (Directory)
		return {.Kind = ContentEntryKind::Directory};
	static const std::unordered_map<string, resource::AssetType> AssetExtensions{
		{".png", resource::AssetType::Texture2D},
		{".jpg", resource::AssetType::Texture2D},
		{".jpeg", resource::AssetType::Texture2D},
		{".tga", resource::AssetType::Texture2D},
		{".bmp", resource::AssetType::Texture2D},
		{".hdr", resource::AssetType::Texture2D},
		{".gltf", resource::AssetType::Model},
		{".glb", resource::AssetType::Model},
		{".obj", resource::AssetType::Model},
		{".fbx", resource::AssetType::Model},
		{".dae", resource::AssetType::Model},
		{".material", resource::AssetType::Material},
		{".materialinstance", resource::AssetType::MaterialInstance},
		{".mesh", resource::AssetType::StaticMesh},
		{".skinnedmesh", resource::AssetType::SkeletalMesh},
		{".skeleton", resource::AssetType::Skeleton},
		{".animation", resource::AssetType::AnimationClip},
		{".animationgraph", resource::AssetType::AnimationGraph},
		{".retarget", resource::AssetType::RetargetProfile},
		{".glsl", resource::AssetType::ShaderSource},
		{".vert", resource::AssetType::ShaderSource},
		{".frag", resource::AssetType::ShaderSource},
		{".comp", resource::AssetType::ShaderSource}};
	const auto Asset = AssetExtensions.find(string(Extension));
	if (Asset != AssetExtensions.end())
		return {.Kind = ContentEntryKind::Asset, .Type = Asset->second};
	if (Extension == ".scene" || Extension == ".enginelevel")
		return {.Kind = ContentEntryKind::Scene};
	if (Extension == ".h" || Extension == ".cpp" || Extension == ".inl")
		return {.Kind = ContentEntryKind::SourceFile};
	return {};
}

[[nodiscard]] bool IsHidden(const std::filesystem::path &Path)
{
	const DWORD Attributes = GetFileAttributesW(Path.c_str());
	return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
}
} // namespace

const ContentEntry *AssetRegistrySnapshot::Find(const resource::AssetID &ID) const noexcept
{
	const auto Entry = std::ranges::find(this->Entries, ID, &ContentEntry::ID);
	return Entry == this->Entries.end() ? nullptr : &*Entry;
}

const ContentEntry *AssetRegistrySnapshot::FindByVirtualPath(const string_view VirtualPath) const noexcept
{
	const auto Entry = std::ranges::find(this->Entries, VirtualPath, &ContentEntry::VirtualPath);
	return Entry == this->Entries.end() ? nullptr : &*Entry;
}

void AssetRegistrySnapshot::SearchInto(const string_view Query, std::vector<const ContentEntry *> &Result) const
{
	const string Filter = Lowercase(string(Query));
	Result.clear();
	Result.reserve(this->Entries.size());
	for (const ContentEntry &Entry : this->Entries)
	{
		if (Filter.empty() || Lowercase(Entry.RelativePath.generic_string()).find(Filter) != string::npos)
			Result.push_back(&Entry);
	}
}
std::vector<const ContentEntry *> AssetRegistrySnapshot::Search(const string_view Query) const
{
	std::vector<const ContentEntry *> Result;
	this->SearchInto(Query, Result);
	return Result;
}

AssetRegistry::AssetRegistry(std::filesystem::path ContentRoot)
	: ContentRoot(std::filesystem::absolute(std::move(ContentRoot)).lexically_normal())
{
	this->ChangesScratch.reserve(256);
}

AssetRegistry::~AssetRegistry()
{
	this->StopWatching();
	this->WaitForRefresh();
}

void AssetRegistry::StartWatching()
{
	this->Watcher.Start(this->ContentRoot);
}

void AssetRegistry::StopWatching() noexcept
{
	this->Watcher.Stop();
}

void AssetRegistry::RequestRefresh(core::threading::TaskScheduler &Scheduler, const bool Force)
{
	this->Watcher.DrainChangesInto(this->ChangesScratch);
	if (this->PendingRefresh.valid())
		return;
	const uint64 ChangeGeneration = this->Watcher.GetChangeGeneration();
	if (!Force && this->Snapshot.Revision != 0 && ChangeGeneration == this->Snapshot.ObservedChangeGeneration)
		return;
	const uint64 OverflowCount = this->Watcher.GetOverflowCount();
	string WatcherDiagnostic = this->Watcher.GetDiagnostic();
	const std::filesystem::path Root = this->ContentRoot;
	this->PendingRefresh = Scheduler.Submit([Root, ChangeGeneration, OverflowCount, WatcherDiagnostic = std::move(WatcherDiagnostic)]()
											{ return AssetRegistry::Scan(Root, ChangeGeneration, OverflowCount, WatcherDiagnostic); },
											core::threading::TaskPriority::Background);
}

bool AssetRegistry::PollRefresh()
{
	if (!this->PendingRefresh.valid() || this->PendingRefresh.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		return false;
	}
	AssetRegistrySnapshot Replacement = this->PendingRefresh.get();
	Replacement.Revision = this->Snapshot.Revision + 1U;
	this->Snapshot = std::move(Replacement);
	return true;
}

void AssetRegistry::WaitForRefresh() noexcept
{
	if (!this->PendingRefresh.valid())
		return;
	try
	{
		AssetRegistrySnapshot Replacement = this->PendingRefresh.get();
		Replacement.Revision = this->Snapshot.Revision + 1U;
		this->Snapshot = std::move(Replacement);
	}
	catch (...)
	{
	}
}

const AssetRegistrySnapshot &AssetRegistry::GetSnapshot() const noexcept
{
	return this->Snapshot;
}

const std::filesystem::path &AssetRegistry::GetContentRoot() const noexcept
{
	return this->ContentRoot;
}

bool AssetRegistry::HasPendingRefresh() const noexcept
{
	return this->PendingRefresh.valid();
}

bool AssetRegistry::HasUnappliedChanges() const noexcept
{
	return this->Watcher.GetChangeGeneration() != this->Snapshot.ObservedChangeGeneration;
}

void AssetRegistry::PublishTo(resource::AssetManager &Assets) const
{
	std::vector<resource::AssetPublication> Publications;
	for (const ContentEntry &Entry : this->Snapshot.Entries)
	{
		if (Entry.Kind != ContentEntryKind::Asset || !Entry.AssetType.has_value())
			continue;
		Publications.push_back({.ID = Entry.ID,
								.CanonicalPath = this->ContentRoot / Entry.RelativePath,
								.VirtualPath = Entry.VirtualPath,
								.Type = *Entry.AssetType,
								.Dependencies = Entry.Dependencies});
	}
	Assets.PublishAssetRegistry(Publications);
}

AssetRegistrySnapshot AssetRegistry::Scan(const std::filesystem::path &ContentRoot, const uint64 ChangeGeneration,
										  const uint64 OverflowCount, string WatcherDiagnostic)
{
	AssetRegistrySnapshot Result{.ObservedChangeGeneration = ChangeGeneration, .WatcherOverflowCount = OverflowCount};
	if (!WatcherDiagnostic.empty())
		Result.Diagnostics.push_back(std::move(WatcherDiagnostic));
	if (OverflowCount != 0)
		Result.Diagnostics.push_back("Content change notification buffer overflowed; registry recovered with a complete rescan");

	std::unordered_multimap<string, std::pair<std::filesystem::path, AssetMetadata>> MetadataByPhysicalIdentity;
	std::error_code Error;
	for (std::filesystem::recursive_directory_iterator MetadataIterator(ContentRoot,
																		std::filesystem::directory_options::skip_permission_denied, Error);
		 !Error && MetadataIterator != std::filesystem::recursive_directory_iterator{}; MetadataIterator.increment(Error))
	{
		const std::filesystem::directory_entry Sidecar = *MetadataIterator;
		if (!Sidecar.is_regular_file(Error) || Lowercase(Sidecar.path().extension().string()) != ".assetmeta")
			continue;
		string Diagnostic;
		std::optional<AssetMetadata> Metadata = AssetMetadataStore::TryLoad(Sidecar.path(), Diagnostic);
		if (!Metadata.has_value())
		{
			Result.Diagnostics.push_back(std::move(Diagnostic));
			continue;
		}
		const string PhysicalSourceIdentity = Metadata->PhysicalSourceIdentity;
		MetadataByPhysicalIdentity.emplace(PhysicalSourceIdentity, std::pair{Sidecar.path(), std::move(*Metadata)});
	}
	Error.clear();
	std::filesystem::recursive_directory_iterator Iterator(ContentRoot, std::filesystem::directory_options::skip_permission_denied, Error);
	const std::filesystem::recursive_directory_iterator End;
	if (Error)
		Result.Diagnostics.push_back("Could not begin content scan: " + Error.message());
	while (Iterator != End)
	{
		const std::filesystem::directory_entry Entry = *Iterator;
		Error.clear();
		const bool Directory = Entry.is_directory(Error);
		if (Error)
		{
			Result.Diagnostics.push_back("Could not inspect content entry '" + Entry.path().string() + "': " + Error.message());
			Iterator.increment(Error);
			continue;
		}
		if (Entry.is_symlink(Error) && Directory)
			Iterator.disable_recursion_pending();
		const std::filesystem::path RelativePath = Entry.path().lexically_relative(ContentRoot);
		const string Extension = Directory ? string{} : Lowercase(Entry.path().extension().string());
		if (!Directory && Extension == ".assetmeta")
		{
			Iterator.increment(Error);
			continue;
		}
		const ClassifiedEntry Classification = Classify(Directory, Extension);
		uint64 Size = 0;
		if (!Directory)
		{
			Error.clear();
			Size = Entry.file_size(Error);
			if (Error)
				Size = 0;
		}
		Error.clear();
		const std::filesystem::file_time_type WriteTime = Entry.last_write_time(Error);
		const int64 WriteCount = Error ? 0 : static_cast<int64>(WriteTime.time_since_epoch().count());
		Error.clear();
		const std::filesystem::perms Permissions = Entry.status(Error).permissions();
		const bool ReadOnly = !Error && (Permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
		const string PhysicalIdentity = Directory ? ReadFileIdentity(Entry.path(), RelativePath, true).ToString()
												  : AssetMetadataStore::CalculatePhysicalSourceIdentity(Entry.path());
		resource::AssetID ID = PhysicalIdentity;
		std::filesystem::path MetadataPath;
		string VirtualPath;
		uint32 ImporterVersion = 0;
		uint32 SchemaVersion = 0;
		string SourceHash;
		std::vector<resource::AssetID> Dependencies;
		if (!Directory && Classification.Kind != ContentEntryKind::Unknown)
		{
			MetadataPath = AssetMetadataStore::GetSidecarPath(Entry.path());
			VirtualPath = "/Game/" + RelativePath.generic_string();
			string MetadataDiagnostic;
			std::optional<AssetMetadata> Metadata;
			if (std::filesystem::is_regular_file(MetadataPath, Error) && !Error)
				Metadata = AssetMetadataStore::TryLoad(MetadataPath, MetadataDiagnostic);
			if (!Metadata.has_value())
			{
				const auto [First, Last] = MetadataByPhysicalIdentity.equal_range(PhysicalIdentity);
				auto Orphan = Last;
				if (First == Last && !MetadataByPhysicalIdentity.empty())
					Result.Diagnostics.push_back("No metadata physical identity matched source '" + Entry.path().string() + "' (" +
												 PhysicalIdentity + ")");
				for (auto Candidate = First; Candidate != Last; ++Candidate)
				{
					const string CandidateText = Candidate->second.first.string();
					if (!CandidateText.ends_with(".assetmeta"))
						continue;
					const std::filesystem::path CandidateSource =
						CandidateText.substr(0, CandidateText.size() - string(".assetmeta").size());
					if (std::filesystem::exists(CandidateSource))
						continue;
					if (Orphan != Last)
					{
						Result.Diagnostics.push_back("Multiple orphaned metadata sidecars match renamed source '" + Entry.path().string() +
													 "'");
						Orphan = Last;
						break;
					}
					Orphan = Candidate;
				}
				if (Orphan != Last)
				{
					Metadata = Orphan->second.second;
					try
					{
						core::io::SecurePath::MoveWithin(ContentRoot, Orphan->second.first.lexically_relative(ContentRoot), ContentRoot,
														 MetadataPath.lexically_relative(ContentRoot), false,
														 "renamed asset metadata publication");
					}
					catch (const std::exception &Exception)
					{
						Result.Diagnostics.push_back("Could not move asset metadata beside renamed source: " + string(Exception.what()));
					}
				}
			}
			try
			{
				const string CurrentHash = AssetMetadataStore::CalculateSourceHash(Entry.path());
				if (!Metadata.has_value())
				{
					Metadata = AssetMetadataStore::Create(Entry.path(), VirtualPath, AssetTypeName(Classification));
				}
				Metadata->AssetType = AssetTypeName(Classification);
				Metadata->VirtualSource = VirtualPath;
				Metadata->PhysicalSourceIdentity = PhysicalIdentity;
				Metadata->SourceHash = CurrentHash;
				AssetMetadataStore::Save(*Metadata, MetadataPath);
				ID = Metadata->ID;
				ImporterVersion = Metadata->ImporterVersion;
				SchemaVersion = Metadata->SchemaVersion;
				SourceHash = Metadata->SourceHash;
				Dependencies = Metadata->Dependencies;
			}
			catch (const std::exception &Exception)
			{
				Result.Diagnostics.push_back("Could not maintain metadata for '" + Entry.path().string() + "': " + Exception.what());
			}
			if (!MetadataDiagnostic.empty())
				Result.Diagnostics.push_back(std::move(MetadataDiagnostic));
		}
		Result.Entries.push_back(
			{.ID = std::move(ID),
			 .RelativePath = RelativePath,
			 .ParentPath = RelativePath.parent_path(),
			 .MetadataPath = MetadataPath.empty() ? std::filesystem::path{} : MetadataPath.lexically_relative(ContentRoot),
			 .VirtualPath = std::move(VirtualPath),
			 .PhysicalSourceIdentity = PhysicalIdentity,
			 .DisplayName = Entry.path().filename().string(),
			 .Extension = Extension,
			 .Kind = Classification.Kind,
			 .AssetType = Classification.Type,
			 .SizeBytes = Size,
			 .LastWriteTime = WriteCount,
			 .ImporterVersion = ImporterVersion,
			 .SchemaVersion = SchemaVersion,
			 .SourceHash = std::move(SourceHash),
			 .Dependencies = std::move(Dependencies),
			 .ReadOnly = ReadOnly,
			 .Hidden = IsHidden(Entry.path())});
		Iterator.increment(Error);
		if (Error)
		{
			Result.Diagnostics.push_back("Content scan skipped an inaccessible subtree: " + Error.message());
			Error.clear();
		}
	}
	std::unordered_map<resource::AssetID, usize> EntryByID;
	for (usize Index = 0; Index < Result.Entries.size(); ++Index)
	{
		if (!EntryByID.emplace(Result.Entries[Index].ID, Index).second)
			Result.Diagnostics.push_back("Duplicate asset ID detected: '" + Result.Entries[Index].ID + "'");
	}
	for (ContentEntry &Source : Result.Entries)
	{
		for (const resource::AssetID &Dependency : Source.Dependencies)
		{
			const auto Target = EntryByID.find(Dependency);
			if (Target == EntryByID.end())
				Result.Diagnostics.push_back("Asset '" + Source.ID + "' has missing dependency '" + Dependency + "'");
			else
				Result.Entries[Target->second].ReverseDependencies.push_back(Source.ID);
		}
	}
	std::ranges::sort(Result.Entries,
					  [](const ContentEntry &Left, const ContentEntry &Right)
					  {
						  if (Left.ParentPath != Right.ParentPath)
							  return Left.ParentPath.generic_string() < Right.ParentPath.generic_string();
						  if (Left.Kind == ContentEntryKind::Directory && Right.Kind != ContentEntryKind::Directory)
							  return true;
						  if (Left.Kind != ContentEntryKind::Directory && Right.Kind == ContentEntryKind::Directory)
							  return false;
						  return Lowercase(Left.DisplayName) < Lowercase(Right.DisplayName);
					  });
	return Result;
}
} // namespace editor::asset
