#pragma once
#include "src/resource/asset/importer/AssetImporter.h"
#include "stb_image.h"
#include "src/pipeline/texture/Texture2D.h"
#include <iostream>
#include "src/util/logger.h"

namespace resource::importer
{
	class Texture2DImporter final : public resource::importer::AssetImporter
	{
	private:
	public:
		virtual bool canImport(const std::filesystem::path& path) override;
		virtual resource::AssetType getAssetType() const override;
		virtual Asset* import(const std::filesystem::path& path) override;
	};
}