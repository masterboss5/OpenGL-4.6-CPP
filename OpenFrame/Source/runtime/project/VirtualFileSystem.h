#pragma once

#include "ProjectDescriptor.h"
#include "Source/core/EngineAPI.h"
#include "Source/types.h"

#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace runtime::project
{
struct ResolvedContentMount final
{
	string VirtualRoot;
	std::filesystem::path PhysicalRoot;
	bool ReadOnly = false;
};

class ENGINE_API VirtualFileSystemException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API VirtualFileSystem final
{
  public:
	VirtualFileSystem(std::filesystem::path ProjectRoot, std::span<const ProjectContentMount> Mounts);

	[[nodiscard]] std::filesystem::path Resolve(string_view VirtualPath) const;
	[[nodiscard]] string MakeVirtual(const std::filesystem::path &PhysicalPath) const;
	[[nodiscard]] const ResolvedContentMount &FindMount(string_view VirtualPath) const;
	[[nodiscard]] std::span<const ResolvedContentMount> GetMounts() const noexcept;

  private:
	std::filesystem::path ProjectRoot;
	std::vector<ResolvedContentMount> Mounts;
};
} // namespace runtime::project
