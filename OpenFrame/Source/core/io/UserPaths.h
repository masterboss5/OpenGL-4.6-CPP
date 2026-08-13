#pragma once

#include "Source/core/EngineAPI.h"

#include <filesystem>

namespace core::io
{
class ENGINE_API UserPaths final
{
  public:
	[[nodiscard]] static std::filesystem::path Documents();
	[[nodiscard]] static std::filesystem::path LocalApplicationData();
	[[nodiscard]] static std::filesystem::path OpenFrameApplicationData();
	[[nodiscard]] static std::filesystem::path OpenFrameProjects();
};
} // namespace core::io
