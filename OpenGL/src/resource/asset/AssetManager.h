#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "src/concepts.h"
#include "src/resource/asset/AssetHandle.h"
#include "src/resource/asset/importer/AssetImporter.h"

namespace resource
{
	class AssetManager final
	{
	public:
		AssetManager();
		~AssetManager() = default;
		AssetManager(const AssetManager&) = delete;
		AssetManager& operator=(const AssetManager&) = delete;

		template<typename ImporterType> requires IsAssetImporter<ImporterType>
		void addAssetImporter()
		{
			auto importer = std::make_unique<ImporterType>();
			const size_t index = static_cast<size_t>(importer->getAssetType());
			if (index >= this->assetImporters.size())
			{
				throw std::out_of_range("Asset importer type exceeds the importer registry");
			}

			std::scoped_lock lock(this->mutex);
			this->assetImporters[index] = std::move(importer);
		}

		template<typename T> requires IsAsset<T>
		[[nodiscard]] AssetHandle<T> getAsset(AssetType type, const std::filesystem::path& path)
		{
			AssetRecord* record = this->loadRecord(type, path, false);
			return AssetHandle<T>(record == nullptr ? nullptr : dynamic_cast<T*>(record->asset.get()), this);
		}

		template<typename T> requires IsAsset<T>
		[[nodiscard]] AssetHandle<T> reloadAsset(AssetType type, const std::filesystem::path& path)
		{
			AssetRecord* record = this->loadRecord(type, path, true);
			return AssetHandle<T>(record == nullptr ? nullptr : dynamic_cast<T*>(record->asset.get()), this);
		}

		[[nodiscard]] bool realizeGpu(const AssetID& id);
		[[nodiscard]] bool realizeGpu(AssetType type, const std::filesystem::path& path);
		void realizeAllPendingGpu();
		[[nodiscard]] size_t reloadChangedAssets();

		[[nodiscard]] const AssetRecord* getRecord(AssetType type, const std::filesystem::path& path) const;
		[[nodiscard]] static std::filesystem::path canonicalizePath(const std::filesystem::path& path);
		[[nodiscard]] static AssetID makeAssetID(AssetType type, const std::filesystem::path& canonicalPath);

	private:
		static constexpr size_t IMPORTER_COUNT = static_cast<size_t>(AssetType::COUNT);

		mutable std::mutex mutex;
		std::unordered_map<AssetID, std::unique_ptr<AssetRecord>> records;
		std::array<std::unique_ptr<importer::AssetImporter>, IMPORTER_COUNT> assetImporters {};

		[[nodiscard]] AssetRecord* loadRecord(AssetType type, const std::filesystem::path& path, bool forceReload);
	};
}
