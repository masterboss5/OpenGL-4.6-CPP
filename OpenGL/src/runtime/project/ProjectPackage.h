#pragma once

#include "PackageFormat.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace runtime::project
{
struct ProjectPackageMount final
{
	util::UUID OperationID;
	util::UUID BuildID;
	util::UUID ProjectID;
	string ProjectName;
	std::filesystem::path PackageRoot;
	std::filesystem::path ContentRoot;
	std::filesystem::path EngineContentRoot;
	std::filesystem::path StartupScene;
	std::filesystem::path GameModule;
};

struct TrustedPackageSigningKey final
{
	string ID;
	uint32 Version = 0;
	std::vector<uint8> PublicKeyBlob;
};

struct ProjectPackageTrustPolicy final
{
	bool RequireSignature = false;
	std::vector<TrustedPackageSigningKey> TrustedKeys;
};

class ENGINE_API ProjectPackageException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API ProjectPackage final
{
  public:
	[[nodiscard]] static bool IsPackage(const std::filesystem::path &Root);
	[[nodiscard]] static ProjectPackageMount Mount(const std::filesystem::path &Root, const std::filesystem::path &CacheRoot = {},
												   const ProjectPackageTrustPolicy &TrustPolicy = {});
};
} // namespace runtime::project
