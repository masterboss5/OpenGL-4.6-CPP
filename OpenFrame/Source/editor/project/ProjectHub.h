#pragma once

#include "Project.h"
#include "Source/types.h"

#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace editor::project
{
struct RecentProject final
{
	string Name;
	std::filesystem::path DescriptorPath;
	int64 LastOpenedMilliseconds = 0;
	bool Available = false;
};

struct NewProjectSpecification final
{
	string Name;
	std::filesystem::path ParentDirectory;
};

class ProjectHubException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ProjectHub final
{
  public:
	ProjectHub();
	ProjectHub(std::filesystem::path ProjectsRoot, std::filesystem::path StateRoot);

	[[nodiscard]] static std::filesystem::path GetDefaultProjectsRoot();
	[[nodiscard]] static std::filesystem::path GetApplicationDataRoot();
	[[nodiscard]] const std::filesystem::path &GetProjectsRoot() const noexcept;
	[[nodiscard]] std::span<const RecentProject> GetRecentProjects() const noexcept;

	void Refresh();
	[[nodiscard]] ProjectDescriptor OpenProject(const std::filesystem::path &DescriptorPath);
	[[nodiscard]] ProjectDescriptor CreateBaseplateProject(const NewProjectSpecification &Specification);
	void RemoveRecent(const std::filesystem::path &DescriptorPath);

  private:
	void LoadRecentProjects();
	void SaveRecentProjects() const;
	void RecordRecent(const ProjectDescriptor &Descriptor);

	std::filesystem::path ProjectsRoot;
	std::filesystem::path StateRoot;
	std::vector<RecentProject> RecentProjects;
};
} // namespace editor::project
