#pragma once

#include "src/core/EngineAPI.h"

#include "src/resource/asset/AssetRecord.h"
#include "src/resource/asset/importer/AssetImportException.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace resource::importer
{
template <IsAsset T> struct ImportedAssetReference final
{
	AssetHandle<T> Handle;
	std::filesystem::path CanonicalPath;
};

struct AssetImportProduct final
{
	AssetType Type = AssetType::Count;
	std::filesystem::path CanonicalPath;
	AssetPtr<resource::Asset> Asset;
	std::vector<AssetDependency> Dependencies;
};

struct AssetImportReservation final
{
	AssetRecordHandle Handle;
	bool WasCreated = false;
};

class ENGINE_API AssetImportContext final
{
  public:
	using ReserveRecordFunction = std::function<AssetImportReservation(AssetType, const std::filesystem::path &)>;
	using ResolveRecordFunction = std::function<AssetRecordHandle(AssetType, const AssetID &)>;
	using RollbackReservationsFunction = std::function<void(std::span<AssetRecord *const>)>;
	using ResolveDependencyPathFunction =
		std::function<std::filesystem::path(AssetType, const std::filesystem::path &, const std::filesystem::path &, string_view)>;

	AssetImportContext(ReserveRecordFunction ReserveRecord, ResolveRecordFunction ResolveRecord,
					   RollbackReservationsFunction RollbackReservations, ResolveDependencyPathFunction ResolveDependencyPath)
		: ReserveRecord(std::move(ReserveRecord)), ResolveRecord(std::move(ResolveRecord)),
		  RollbackReservations(std::move(RollbackReservations)), ResolveDependencyPathCallback(std::move(ResolveDependencyPath))
	{
	}
	~AssetImportContext() noexcept
	{
		if (this->Committed)
			return;
		this->Products.clear();
		try
		{
			this->RollbackReservations(this->CreatedRecords);
		}
		catch (...)
		{
			// Import failure remains primary. Manager rollback callbacks are required to be non-throwing.
		}
	}
	AssetImportContext(const AssetImportContext &) = delete;
	AssetImportContext &operator=(const AssetImportContext &) = delete;

	template <IsAsset T> [[nodiscard]] AssetHandle<T> Reserve(AssetType Type, const std::filesystem::path &CanonicalPath)
	{
		const AssetImportReservation Reservation = this->ReserveRecord(Type, CanonicalPath);
		AssetRecord *const Record = const_cast<AssetRecord *>(Reservation.Handle.Get());
		if (Record == nullptr)
			throw std::logic_error("Asset import reservation returned an empty record");
		if (Reservation.WasCreated)
			this->CreatedRecords.push_back(Record);
		return AssetHandle<T>(Record);
	}

	template <IsAsset T> [[nodiscard]] ImportedAssetReference<T> Resolve(AssetType Type, const AssetID &ID)
	{
		AssetRecordHandle RecordLifetime = this->ResolveRecord(Type, ID);
		AssetRecord *Record = const_cast<AssetRecord *>(RecordLifetime.Get());
		if (Record == nullptr)
			throw AssetUnavailableException(ID, Type, "asset ID could not be resolved during import");
		return {.Handle = AssetHandle<T>(Record), .CanonicalPath = Record->GetCanonicalPath()};
	}

	template <IsAsset T>
	void Stage(AssetType Type, std::filesystem::path CanonicalPath, AssetPtr<T> Asset, std::vector<AssetDependency> Dependencies = {})
	{
		if (Asset == nullptr)
			throw std::invalid_argument("Imported subasset product cannot be null");
		this->Products.push_back({Type, std::move(CanonicalPath), AssetPtr<resource::Asset>(std::move(Asset)), std::move(Dependencies)});
	}

	[[nodiscard]] std::vector<AssetImportProduct> ReleaseProducts() noexcept
	{
		return std::move(this->Products);
	}

	void Commit() noexcept
	{
		this->Committed = true;
		this->CreatedRecords.clear();
	}

	[[nodiscard]] std::filesystem::path ResolveDependencyPath(const AssetType OwnerType, const std::filesystem::path &OwnerPath,
															  const std::filesystem::path &RelativePath, const string_view Role) const
	{
		if (!this->ResolveDependencyPathCallback)
			throw std::logic_error("Asset import context has no secure dependency-path resolver");
		return this->ResolveDependencyPathCallback(OwnerType, OwnerPath, RelativePath, Role);
	}

  private:
	ReserveRecordFunction ReserveRecord;
	ResolveRecordFunction ResolveRecord;
	RollbackReservationsFunction RollbackReservations;
	ResolveDependencyPathFunction ResolveDependencyPathCallback;
	std::vector<AssetImportProduct> Products;
	std::vector<AssetRecord *> CreatedRecords;
	bool Committed = false;
};

struct ENGINE_API AssetImportResult final
{
	AssetImportResult(AssetPtr<resource::Asset> Asset, std::vector<AssetDependency> Dependencies = {});

	AssetPtr<resource::Asset> Asset;
	std::vector<AssetDependency> Dependencies;
};

class ENGINE_API AssetImporter
{
  public:
	virtual ~AssetImporter() = default;

	[[nodiscard]] virtual bool CanImport(const std::filesystem::path &Path) const = 0;
	[[nodiscard]] virtual AssetType GetAssetType() const noexcept = 0;
	[[nodiscard]] virtual AssetImportResult ImportCPU(const std::filesystem::path &Path, AssetImportContext &Context) const = 0;

  protected:
	void ValidateImportRequest(const std::filesystem::path &Path) const;
	[[nodiscard]] std::string ReadTextSource(const std::filesystem::path &Path) const;
	[[nodiscard]] static std::string GetNormalizedExtension(const std::filesystem::path &Path);
};
} // namespace resource::importer
