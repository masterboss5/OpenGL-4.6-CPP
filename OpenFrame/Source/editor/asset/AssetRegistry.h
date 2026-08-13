#pragma once

#include "AssetMetadata.h"
#include "ContentWatcher.h"
#include "Source/core/threading/TaskScheduler.h"
#include "Source/resource/asset/AssetTypes.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <filesystem>
#include <future>
#include <optional>
#include <vector>

namespace resource
{
class AssetManager;
}

namespace editor::asset
{
enum class ContentEntryKind : uint8
{
	Directory,
	Scene,
	Asset,
	SourceFile,
	Unknown
};

struct ContentEntry final
{
	resource::AssetID ID;
	std::filesystem::path RelativePath;
	std::filesystem::path ParentPath;
	std::filesystem::path MetadataPath;
	string VirtualPath;
	string PhysicalSourceIdentity;
	string DisplayName;
	string Extension;
	ContentEntryKind Kind = ContentEntryKind::Unknown;
	std::optional<resource::AssetType> AssetType;
	uint64 SizeBytes = 0;
	int64 LastWriteTime = 0;
	uint32 ImporterVersion = 0;
	uint32 SchemaVersion = 0;
	string SourceHash;
	std::vector<resource::AssetID> Dependencies;
	std::vector<resource::AssetID> ReverseDependencies;
	bool ReadOnly = false;
	bool Hidden = false;
};

struct AssetRegistrySnapshot final
{
	uint64 Revision = 0;
	uint64 ObservedChangeGeneration = 0;
	uint64 WatcherOverflowCount = 0;
	std::vector<ContentEntry> Entries;
	std::vector<string> Diagnostics;

	[[nodiscard]] const ContentEntry *Find(const resource::AssetID &ID) const noexcept;
	[[nodiscard]] const ContentEntry *FindByVirtualPath(string_view VirtualPath) const noexcept;
	void SearchInto(string_view Query, std::vector<const ContentEntry *> &Result) const;
	[[nodiscard]] std::vector<const ContentEntry *> Search(string_view Query) const;
};

class AssetRegistry final
{
  public:
	explicit AssetRegistry(std::filesystem::path ContentRoot);
	~AssetRegistry();

	AssetRegistry(const AssetRegistry &) = delete;
	AssetRegistry &operator=(const AssetRegistry &) = delete;
	AssetRegistry(AssetRegistry &&) = delete;
	AssetRegistry &operator=(AssetRegistry &&) = delete;

	void StartWatching();
	void StopWatching() noexcept;
	void RequestRefresh(core::threading::TaskScheduler &Scheduler, bool Force = false);
	[[nodiscard]] bool PollRefresh();
	void WaitForRefresh() noexcept;

	[[nodiscard]] const AssetRegistrySnapshot &GetSnapshot() const noexcept;
	[[nodiscard]] const std::filesystem::path &GetContentRoot() const noexcept;
	[[nodiscard]] bool HasPendingRefresh() const noexcept;
	[[nodiscard]] bool HasUnappliedChanges() const noexcept;
	void PublishTo(resource::AssetManager &Assets) const;

  private:
	[[nodiscard]] static AssetRegistrySnapshot Scan(const std::filesystem::path &ContentRoot, uint64 ChangeGeneration, uint64 OverflowCount,
													string WatcherDiagnostic);

	std::filesystem::path ContentRoot;
	ContentWatcher Watcher;
	AssetRegistrySnapshot Snapshot;
	std::vector<ContentChange> ChangesScratch;
	std::future<AssetRegistrySnapshot> PendingRefresh;
};
} // namespace editor::asset
