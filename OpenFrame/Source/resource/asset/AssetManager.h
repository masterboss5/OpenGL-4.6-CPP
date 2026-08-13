#pragma once

#include "Source/core/EngineAPI.h"

#include "Source/concepts.h"
#include "Source/resource/asset/AssetHandle.h"
#include "Source/resource/asset/importer/AssetImporter.h"

#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace resource
{
class AssetManager;

class ENGINE_API AssetLoadTransaction final
{
  public:
	AssetLoadTransaction(const AssetLoadTransaction &) = delete;
	AssetLoadTransaction &operator=(const AssetLoadTransaction &) = delete;
	AssetLoadTransaction(AssetLoadTransaction &&) = delete;
	AssetLoadTransaction &operator=(AssetLoadTransaction &&) = delete;
	~AssetLoadTransaction();

	void Commit() noexcept;
	void Rollback();
	[[nodiscard]] bool IsActive() const noexcept;

  private:
	struct State;
	std::unique_ptr<State> TransactionState;

	explicit AssetLoadTransaction(AssetManager &Manager);
	friend class AssetManager;
};

class ENGINE_API GeneratedAssetStage final
{
  public:
	GeneratedAssetStage() = default;
	GeneratedAssetStage(const GeneratedAssetStage &) = delete;
	GeneratedAssetStage &operator=(const GeneratedAssetStage &) = delete;
	GeneratedAssetStage(GeneratedAssetStage &&) noexcept = default;
	GeneratedAssetStage &operator=(GeneratedAssetStage &&) noexcept = default;

	template <IsAsset T> [[nodiscard]] AssetHandle<T> GetHandle() const
	{
		if (!this->Record)
			throw std::logic_error("Generated asset stage is empty");
		return AssetHandle<T>(this->Record.Record);
	}
	[[nodiscard]] const AssetID &GetID() const;
	[[nodiscard]] bool IsCommitted() const noexcept;

  private:
	AssetRecordHandle Record;
	std::vector<AssetID> Dependencies;
	bool Committed = false;

	GeneratedAssetStage(AssetRecordHandle Record, std::vector<AssetID> Dependencies)
		: Record(std::move(Record)), Dependencies(std::move(Dependencies))
	{
	}
	friend class AssetManager;
};

struct GPURealizationBudget final
{
	uint32 MaximumAssets = 32;
	std::chrono::microseconds MaximumWallTime{2'000};

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->MaximumAssets != 0 && this->MaximumWallTime.count() > 0;
	}
};

struct GPURealizationBatchResult final
{
	uint32 Attempted = 0;
	uint32 Realized = 0;
	uint32 Failed = 0;
	usize Remaining = 0;
};

struct AssetRecordSnapshot final
{
	AssetID ID;
	std::filesystem::path CanonicalPath;
	AssetType Type = AssetType::Count;
	AssetLoadState State = AssetLoadState::Unloaded;
	string Error;
	std::vector<AssetID> Dependencies;
	std::vector<AssetID> ReverseDependencies;
	uint64 PublishedGeneration = 0;
	uint32 StrongReferences = 0;
};

struct AssetPublication final
{
	AssetID ID;
	std::filesystem::path CanonicalPath;
	string VirtualPath;
	AssetType Type = AssetType::Count;
	std::vector<AssetID> Dependencies;
};

enum class GeneratedAssetRetirementStatus : uint8
{
	Retired,
	NotGenerated,
	Referenced,
	HasDependents,
	Busy
};

struct GeneratedAssetRetirement final
{
	AssetPublication Publication;
	AssetPtr<Asset> Asset;
	bool WasRegistryPublished = false;
};

struct GeneratedAssetRetirementAttempt final
{
	GeneratedAssetRetirementStatus Status = GeneratedAssetRetirementStatus::NotGenerated;
	std::optional<GeneratedAssetRetirement> Retirement;
};

class ENGINE_API AssetManager final
{
  public:
	explicit AssetManager(std::filesystem::path RootPath = {});
	~AssetManager();
	AssetManager(const AssetManager &) = delete;
	AssetManager &operator=(const AssetManager &) = delete;

	template <IsAssetImporter ImporterType> void AddAssetImporter()
	{
		auto Importer = std::make_shared<ImporterType>();
		const usize Index = static_cast<usize>(Importer->GetAssetType());
		if (Index >= this->AssetImporters.size())
		{
			throw std::out_of_range("Asset importer type exceeds the importer registry");
		}

		std::scoped_lock Lock(this->Mutex);
		this->AssetImporters[Index] = std::move(Importer);
	}

	template <IsAsset T> [[nodiscard]] AssetHandle<T> GetAsset(AssetType Type, const std::filesystem::path &Path)
	{
		AssetRecordHandle Record = this->LoadRecord(Type, Path, false);
		if (Record)
			(void)Record->Pin<T>();
		return AssetHandle<T>(Record.Record);
	}
	template <IsAssetWithStaticType T>
	[[nodiscard]] GeneratedAssetStage StageGeneratedAsset(AssetID ID, const std::filesystem::path &Path, AssetPtr<T> Asset,
														  std::vector<AssetID> Dependencies = {})
	{
		if (!Asset)
			throw std::invalid_argument("Generated asset staging requires an owned asset");
		return this->StageGeneratedAssetRecord(std::move(ID), Path, T::AssetType, std::move(Asset), std::move(Dependencies));
	}
	void CommitGeneratedAssets(std::span<GeneratedAssetStage *const> Stages);

	template <IsAssetWithStaticType T> [[nodiscard]] AssetHandle<T> GetAsset(const std::filesystem::path &Path)
	{
		return this->GetAsset<T>(T::AssetType, Path);
	}

	template <IsAssetWithStaticType T> [[nodiscard]] AssetHandle<T> GetAssetByID(const AssetID &ID)
	{
		AssetPublication Publication;
		{
			std::scoped_lock Lock(this->Mutex);
			const auto Published = this->PublishedAssets.find(ID);
			if (Published != this->PublishedAssets.end())
				Publication = Published->second;
			else
			{
				const auto Generated = this->GeneratedAssets.find(ID);
				if (Generated == this->GeneratedAssets.end())
					throw AssetUnavailableException(ID, T::AssetType, "asset ID is not registered");
				Publication = Generated->second;
			}
		}
		if (Publication.Type != T::AssetType)
			throw AssetTypeMismatchException(ID, Publication.Type);
		return this->GetAsset<T>(Publication.CanonicalPath);
	}

	template <IsAssetWithStaticType T> [[nodiscard]] AssetHandle<T> GetAssetByVirtualPath(const string_view VirtualPath)
	{
		AssetID ID;
		{
			std::scoped_lock Lock(this->Mutex);
			const auto Published = this->PublishedIDsByVirtualPath.find(string(VirtualPath));
			if (Published == this->PublishedIDsByVirtualPath.end())
				throw AssetUnavailableException({}, T::AssetType, "virtual asset path is not registered: " + string(VirtualPath));
			ID = Published->second;
		}
		return this->GetAssetByID<T>(ID);
	}

	template <IsAsset T> [[nodiscard]] AssetHandle<T> ReloadAsset(AssetType Type, const std::filesystem::path &Path)
	{
		AssetRecordHandle Record = this->LoadRecord(Type, Path, true);
		if (Record)
			(void)Record->Pin<T>();
		return AssetHandle<T>(Record.Record);
	}
	[[nodiscard]] AssetHandle<Asset> ReloadAssetByID(const AssetID &ID);

	[[nodiscard]] bool RealizeGPU(pipeline::device::Device &Device, const AssetID &ID);
	[[nodiscard]] bool RealizeGPU(pipeline::device::Device &Device, AssetType Type, const std::filesystem::path &Path);
	[[nodiscard]] GPURealizationBatchResult ProcessPendingGPU(pipeline::device::Device &Device, GPURealizationBudget Budget);
	[[nodiscard]] AssetLoadTransaction BeginLoadTransaction();
	void RealizeAllPendingGPU(pipeline::device::Device &Device);
	[[nodiscard]] usize ReloadChangedAssets();
	void PublishAssetRegistry(std::span<const AssetPublication> Assets);
	[[nodiscard]] GeneratedAssetRetirementAttempt TryRetireGeneratedAsset(const AssetID &ID);
	void RestoreRetiredGeneratedAsset(GeneratedAssetRetirement Retirement);
	template <IsAssetWithStaticType T>
	[[nodiscard]] AssetHandle<T> PublishGeneratedAsset(AssetID ID, const std::filesystem::path &Path, AssetPtr<T> Asset,
													   std::vector<AssetID> Dependencies = {})
	{
		if (!Asset)
			throw std::invalid_argument("Generated asset publication requires an owned asset");
		AssetRecordHandle Record =
			this->PublishGeneratedAssetRecord(std::move(ID), Path, T::AssetType, std::move(Asset), std::move(Dependencies));
		return AssetHandle<T>(Record.Record);
	}

	[[nodiscard]] AssetRecordHandle GetRecord(AssetType Type, const std::filesystem::path &Path) const;
	[[nodiscard]] AssetRecordHandle GetRecord(const AssetID &ID) const;
	[[nodiscard]] std::vector<AssetRecordSnapshot> SnapshotRecords() const;
	[[nodiscard]] std::optional<AssetRecordSnapshot> SnapshotRecord(const AssetID &ID) const;
	[[nodiscard]] std::optional<AssetPublication> ResolveVirtualPath(string_view VirtualPath) const;
	[[nodiscard]] const std::filesystem::path &GetRootPath() const noexcept;
	[[nodiscard]] std::filesystem::path ResolvePath(const std::filesystem::path &Path) const;
	[[nodiscard]] static std::filesystem::path CanonicalizePath(const std::filesystem::path &Path);
	[[nodiscard]] static AssetID MakeAssetID(AssetType Type, const std::filesystem::path &CanonicalPath);

  private:
	static constexpr usize ImporterCount = static_cast<usize>(AssetType::Count);

	mutable std::mutex Mutex;
	mutable std::shared_mutex LoadTransactionMutex;
	mutable std::mutex LoadGraphMutex;
	std::filesystem::path RootPath;
	std::unordered_map<AssetID, AssetRecord *> Records;
	std::array<std::shared_ptr<importer::AssetImporter>, ImporterCount> AssetImporters{};
	std::unordered_map<AssetID, std::unordered_set<AssetID>> ReverseDependencies;
	std::unordered_map<AssetID, std::filesystem::path> DependencyPaths;
	std::unordered_map<AssetID, AssetPublication> PublishedAssets;
	std::unordered_map<AssetID, AssetID> PublishedIDsByPathKey;
	std::unordered_map<string, AssetID> PublishedIDsByVirtualPath;
	std::unordered_map<AssetID, AssetPublication> GeneratedAssets;
	std::unordered_map<AssetID, AssetID> GeneratedIDsByPathKey;
	std::deque<AssetID> PendingGPUAssetIDs;
	std::unordered_map<AssetID, std::thread::id> LoadOwners;
	std::unordered_map<std::thread::id, AssetID> WaitingLoads;

	[[nodiscard]] AssetRecordHandle LoadRecord(AssetType Type, const std::filesystem::path &Path, bool ForceReload);
	[[nodiscard]] importer::AssetImportReservation ReserveRecord(AssetType Type, const std::filesystem::path &Path);
	[[nodiscard]] AssetRecordHandle ResolvePublishedRecord(AssetType Type, const AssetID &ID);
	[[nodiscard]] AssetRecordHandle PublishGeneratedAssetRecord(AssetID ID, const std::filesystem::path &Path, AssetType Type,
																AssetPtr<Asset> Asset, std::vector<AssetID> Dependencies);
	[[nodiscard]] GeneratedAssetStage StageGeneratedAssetRecord(AssetID ID, const std::filesystem::path &Path, AssetType Type,
																AssetPtr<Asset> Asset, std::vector<AssetID> Dependencies);
	void ReplaceDependencies(AssetRecord &Record, std::vector<AssetID> Dependencies,
							 std::unordered_map<AssetID, std::filesystem::file_time_type> DependencyWriteTimes);
	void QueueGPURealization(AssetRecord &Record);
	[[nodiscard]] AssetID ResolveAssetID(AssetType Type, const std::filesystem::path &CanonicalPath) const;
	void RegisterLoadOwner(const AssetID &ID);
	void ReleaseLoadOwner(const AssetID &ID) noexcept;
	void BeginLoadWait(const AssetID &ID, AssetType Type, const std::filesystem::path &Path);
	void EndLoadWait() noexcept;
	void RollbackImportReservations(std::span<AssetRecord *const> Records) noexcept;
	friend class AssetLoadTransaction;
	friend class AssetLoadSharedAccess;
};
} // namespace resource
