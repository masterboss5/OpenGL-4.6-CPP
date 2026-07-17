#pragma once

#include "src/resource/asset/importer/AssetImporter.h"

namespace resource::importer
{
	class Texture2DImporter final : public AssetImporter
	{
	public:
		[[nodiscard]] bool canImport(const std::filesystem::path& path) const override;
		[[nodiscard]] AssetType getAssetType() const noexcept override;
		[[nodiscard]] AssetImportResult importCpu(const std::filesystem::path& path) const override;
	};
}
