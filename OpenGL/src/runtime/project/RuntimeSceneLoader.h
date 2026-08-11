#pragma once

#include "RuntimeSceneBinary.h"

#include "src/core/EngineAPI.h"
#include "src/resource/asset/AssetManager.h"
#include "src/scene/Scene.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace runtime::project
{
struct LoadedRuntimeScene final
{
	util::UUID ID;
	string Name;
	std::unique_ptr<world::Scene> Scene;
};

class ENGINE_API RuntimeSceneLoadException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API RuntimeSceneLoader final
{
  public:
	static constexpr uint32 CurrentFormatVersion = RuntimeSceneBinary::FormatVersion;

	[[nodiscard]] static LoadedRuntimeScene Load(const std::filesystem::path &Path, resource::AssetManager &Assets);
};
} // namespace runtime::project
