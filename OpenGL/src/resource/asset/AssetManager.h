#pragma once
#include "src/resource/asset/AssetHandle.h"
#include <unordered_map>
#include <array>
#include "src/resource/asset/importer/AssetImporter.h"
#include "src/Types.h"
#include "src/util/logger.h"
#include <filesystem>

#define MAX_ASSET_IMPORTERS (256)

namespace resource
{
	class AssetManager final
	{
	private:
		std::unordered_map<util::UUID, Asset*> assetCache;
		std::unordered_map<std::string, util::UUID> pathIndex;
		std::array<importer::AssetImporter*, MAX_ASSET_IMPORTERS> assetImporters = {};
	public:
		AssetManager();
		~AssetManager();
		AssetManager(const AssetManager&) = delete;
		AssetManager& operator=(const AssetManager&) = delete;
		AssetManager(AssetManager&&) = delete;
		AssetManager& operator=(AssetManager&&) = delete;

		template<typename ImporterType> requires IsAssetImporter<ImporterType>
		void addAssetImporter()
		{
			ImporterType* importer = new ImporterType();
			const uint64 index = static_cast<uint64>(importer->getAssetType());

			if (index >= MAX_ASSET_IMPORTERS)
			{
				LOG_ERROR("Asset importer entries exceeded maximum amount")
				delete importer;

				return;
			}
			 
			delete assetImporters[index];
			assetImporters[index] = importer;
		}

		template<typename T> requires IsAsset<T>
		AssetHandle<T> loadAsset(AssetType type, const std::filesystem::path& path)
		{
			const uint64 index = static_cast<int>(type);

			if (index >= MAX_ASSET_IMPORTERS)
			{
				LOG_ERROR("Asset importer entries exceeded maximum amount")

				return AssetHandle<T>(nullptr, this);
			}

			importer::AssetImporter* importer = this->assetImporters[index];
			
			if (importer == nullptr)
			{
				LOG_ERROR("No asset importer found for type: " + std::to_string(index))
				return AssetHandle<T>(nullptr, this);
			}
			else
			{
				Asset* asset = importer->import(path);
				this->assetCache[asset->getUUID()] = asset;
				pathIndex[path.string()] = asset->getUUID();

				return AssetHandle<T>(static_cast<T*>(asset), this);
			}
		}

		template<typename T> requires IsAsset<T>
		AssetHandle<T> getAsset(AssetType type, const std::filesystem::path& path)
		{
			const std::string key = path.string();
			auto it = pathIndex.find(key);

			if (it != pathIndex.end())
			{
				auto it2 = assetCache.find(it->second);
				if (it2 != assetCache.end())
				{
					return AssetHandle<T>(static_cast<T*>(it2->second), this);
				}
			}

			return this->loadAsset(type, path);
		}

		void cleanupAssetImporters();
	};
}