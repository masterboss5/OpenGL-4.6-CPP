#pragma once

#include "src/resource/asset/AssetTypes.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

namespace editor::asset
{
struct DerivedAssetMetadata final
{
	string Key;
	resource::AssetID ID;
	resource::AssetType Type = resource::AssetType::Model;
};

struct AssetMetadata final
{
	static constexpr uint32 CurrentFormatVersion = 1;

	uint32 FormatVersion = CurrentFormatVersion;
	resource::AssetID ID;
	string AssetType;
	uint32 ImporterVersion = 1;
	uint32 SchemaVersion = 1;
	string VirtualSource;
	string PhysicalSourceIdentity;
	string SourceHash;
	std::vector<resource::AssetID> Dependencies;
	std::vector<DerivedAssetMetadata> DerivedAssets;
};

class AssetMetadataStore final
{
  public:
	[[nodiscard]] static std::filesystem::path GetSidecarPath(const std::filesystem::path &AssetPath);
	[[nodiscard]] static string CalculatePhysicalSourceIdentity(const std::filesystem::path &AssetPath);
	[[nodiscard]] static string CalculateSourceHash(const std::filesystem::path &AssetPath);
	[[nodiscard]] static std::optional<string> InferAssetTypeName(const std::filesystem::path &AssetPath);
	[[nodiscard]] static AssetMetadata Create(const std::filesystem::path &AssetPath, string VirtualSource, string AssetType);
	[[nodiscard]] static std::optional<AssetMetadata> TryLoad(const std::filesystem::path &SidecarPath, string &Diagnostic);
	static void Save(const AssetMetadata &Metadata, const std::filesystem::path &SidecarPath);
};
} // namespace editor::asset
