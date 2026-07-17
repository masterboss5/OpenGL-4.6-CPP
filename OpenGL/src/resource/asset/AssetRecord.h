#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "src/resource/Asset.h"
#include "src/resource/asset/AssetTypes.h"

namespace resource
{
	using AssetID = std::string;

	enum class AssetLoadState : uint8
	{
		Unloaded,
		LoadingCpu,
		CpuReady,
		RealizingGpu,
		Ready,
		Failed
	};

	struct AssetDependency final
	{
		AssetType type;
		std::filesystem::path path;
	};

	struct AssetRecord final
	{
		AssetID id;
		std::filesystem::path canonicalPath;
		std::filesystem::file_time_type sourceWriteTime {};
		AssetType type;
		AssetLoadState state = AssetLoadState::Unloaded;
		std::string error;
		std::vector<AssetID> dependencies;
		AssetPtr<Asset> asset;
	};
}
