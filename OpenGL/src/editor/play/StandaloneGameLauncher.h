#pragma once

#include "src/types.h"

#include <filesystem>
#include <stdexcept>

namespace editor::play
{
struct StandaloneGameLaunchSpecification final
{
	std::filesystem::path Executable;
	std::filesystem::path ProjectDescriptor;
	std::filesystem::path Scene;
};

class StandaloneGameLaunchException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class StandaloneGameLauncher final
{
  public:
	[[nodiscard]] static uint32 Launch(const StandaloneGameLaunchSpecification &Specification);
};
} // namespace editor::play
