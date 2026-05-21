#pragma once
#include <string>
#include <filesystem>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "src/resource/asset/AssetTypes.h"
#include "src/resource/Asset.h"

namespace resource::importer
{
	/*Interface*/
	class AssetImporter
	{
	private:
	public:
		virtual ~AssetImporter() = default;

		virtual bool canImport(const std::filesystem::path& path) = 0;
		virtual resource::AssetType getAssetType() const = 0;
		virtual Asset* import(const std::filesystem::path& path) = 0;
	};
}