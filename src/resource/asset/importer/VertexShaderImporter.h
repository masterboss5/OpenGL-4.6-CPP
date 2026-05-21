#pragma once
#include "src/resource/asset/importer/AssetImporter.h"
#include "src/util/logger.h"
#include "src/util/files.h"
#include "src/pipeline/shader/VertexShader.h"

namespace resource::importer
{
	class VertexShaderImporter final : public resource::importer::AssetImporter
	{
	public:
		virtual bool canImport(const std::filesystem::path& path) override;
		virtual resource::AssetType getAssetType() const override;
		virtual Asset* import(const std::filesystem::path& path) override;
	};
}