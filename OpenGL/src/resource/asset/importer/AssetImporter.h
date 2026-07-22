#pragma once

#include "src/resource/asset/AssetRecord.h"
#include "src/resource/asset/importer/AssetImportException.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace resource::importer
{
struct AssetImportProduct final
{
	AssetType Type = AssetType::Count;
	std::filesystem::path CanonicalPath;
	AssetPtr<resource::Asset> Asset;
	std::vector<AssetDependency> Dependencies;
};

class AssetImportContext final
{
  public:
	using ReserveRecordFunction = std::function<AssetRecord *(AssetType, const std::filesystem::path &)>;

	explicit AssetImportContext(ReserveRecordFunction ReserveRecord) : ReserveRecord(std::move(ReserveRecord))
	{
	}

	template <IsAsset T> [[nodiscard]] AssetHandle<T> Reserve(AssetType Type, const std::filesystem::path &CanonicalPath)
	{
		return AssetHandle<T>(this->ReserveRecord(Type, CanonicalPath));
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

  private:
	ReserveRecordFunction ReserveRecord;
	std::vector<AssetImportProduct> Products;
};

struct AssetImportResult final
{
	AssetImportResult(AssetPtr<resource::Asset> Asset, std::vector<AssetDependency> Dependencies = {});

	AssetPtr<resource::Asset> Asset;
	std::vector<AssetDependency> Dependencies;
};

class AssetImporter
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
