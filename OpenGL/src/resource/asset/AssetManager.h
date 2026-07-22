#pragma once

#include "src/concepts.h"
#include "src/resource/asset/AssetHandle.h"
#include "src/resource/asset/importer/AssetImporter.h"

#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace resource
{
class AssetManager final
{
  public:
	AssetManager();
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
		AssetRecord *Record = this->LoadRecord(Type, Path, false);
		if (Record != nullptr)
			(void)Record->Pin<T>();
		return AssetHandle<T>(Record);
	}

	template <IsAssetWithStaticType T> [[nodiscard]] AssetHandle<T> GetAsset(const std::filesystem::path &Path)
	{
		return this->GetAsset<T>(T::AssetType, Path);
	}

	template <IsAsset T> [[nodiscard]] AssetHandle<T> ReloadAsset(AssetType Type, const std::filesystem::path &Path)
	{
		AssetRecord *Record = this->LoadRecord(Type, Path, true);
		if (Record != nullptr)
			(void)Record->Pin<T>();
		return AssetHandle<T>(Record);
	}

	[[nodiscard]] bool RealizeGPU(pipeline::device::Device &Device, const AssetID &ID);
	[[nodiscard]] bool RealizeGPU(pipeline::device::Device &Device, AssetType Type, const std::filesystem::path &Path);
	void RealizeAllPendingGPU(pipeline::device::Device &Device);
	[[nodiscard]] usize ReloadChangedAssets();

	[[nodiscard]] const AssetRecord *GetRecord(AssetType Type, const std::filesystem::path &Path) const;
	[[nodiscard]] static std::filesystem::path CanonicalizePath(const std::filesystem::path &Path);
	[[nodiscard]] static AssetID MakeAssetID(AssetType Type, const std::filesystem::path &CanonicalPath);

  private:
	static constexpr usize ImporterCount = static_cast<usize>(AssetType::Count);

	mutable std::mutex Mutex;
	std::unordered_map<AssetID, AssetRecord *> Records;
	std::array<std::shared_ptr<importer::AssetImporter>, ImporterCount> AssetImporters{};
	std::unordered_map<AssetID, std::unordered_set<AssetID>> ReverseDependencies;
	std::unordered_map<AssetID, std::filesystem::path> DependencyPaths;

	[[nodiscard]] AssetRecord *LoadRecord(AssetType Type, const std::filesystem::path &Path, bool ForceReload);
	[[nodiscard]] AssetRecord *ReserveRecord(AssetType Type, const std::filesystem::path &Path);
};
} // namespace resource
