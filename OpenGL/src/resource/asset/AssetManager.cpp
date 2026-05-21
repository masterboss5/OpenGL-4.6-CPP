#include "AssetManager.h"

resource::AssetManager::AssetManager()
{
	this->assetImporters.fill(nullptr);

}

resource::AssetManager::~AssetManager()
{
	this->cleanupAssetImporters();
}

void resource::AssetManager::cleanupAssetImporters()
{
	for (auto& importer : this->assetImporters)
	{
		delete importer;
	}

	for (auto& [id, second] : this->assetCache)
	{
		delete second;
	}
}
