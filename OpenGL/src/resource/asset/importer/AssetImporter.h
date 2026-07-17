#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "src/resource/asset/AssetRecord.h"
#include "src/resource/asset/importer/AssetImportException.h"

namespace resource::importer
{
	struct AssetImportResult final
	{
		AssetImportResult(AssetPtr<Asset> asset, std::vector<AssetDependency> dependencies = {});

		AssetPtr<Asset> asset;
		std::vector<AssetDependency> dependencies;
	};

	class AssetImporter
	{
	public:
		virtual ~AssetImporter() = default;

		virtual bool canImport(const std::filesystem::path& path) const = 0;
		virtual AssetType getAssetType() const noexcept = 0;
		virtual AssetImportResult importCpu(const std::filesystem::path& path) const = 0;

	protected:
		void validateImportRequest(const std::filesystem::path& path) const;
		[[nodiscard]] std::string readTextSource(const std::filesystem::path& path) const;
		[[nodiscard]] static std::string getNormalizedExtension(const std::filesystem::path& path);
	};
}
