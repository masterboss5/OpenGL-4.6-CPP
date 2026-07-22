#pragma once

#include "src/resource/asset/importer/AssetImporter.h"

namespace resource::importer
{
class ModelAssetImporter final : public AssetImporter
{
  public:
	[[nodiscard]] bool CanImport(const std::filesystem::path &Path) const override;
	[[nodiscard]] AssetType GetAssetType() const noexcept override;
	[[nodiscard]] AssetImportResult ImportCPU(const std::filesystem::path &Path, AssetImportContext &Context) const override;
};
} // namespace resource::importer
