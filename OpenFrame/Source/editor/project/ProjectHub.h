#pragma once

#include "Project.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace editor::project
{
struct ProjectThumbnailImage final
{
	uint32 Width = 0;
	uint32 Height = 0;
	std::vector<uint8> Pixels;

	[[nodiscard]] bool IsValid() const noexcept;
};

struct RecentProject final
{
	util::UUID ID;
	string Name;
	std::filesystem::path DescriptorPath;
	int64 LastOpenedMilliseconds = 0;
	int64 LastEditedMilliseconds = 0;
	std::filesystem::path ThumbnailPath;
	uint64 ThumbnailRevision = 0;
	ProjectThumbnailImage Thumbnail;
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
	void MarkProjectEdited(const ProjectDescriptor &Descriptor);
	void UpdateProjectThumbnail(const ProjectDescriptor &Descriptor, uint32 SourceWidth, uint32 SourceHeight,
								std::span<const uint8> SourcePixels, bool SourceRowsAreBottomUp);
	void RemoveRecent(const std::filesystem::path &DescriptorPath);

  private:
	void LoadRecentProjects();
	void SaveRecentProjects() const;
	void RecordRecent(const ProjectDescriptor &Descriptor);
	void RefreshRecentProject(RecentProject &Recent, const ProjectDescriptor *KnownDescriptor = nullptr);

	std::filesystem::path ProjectsRoot;
	std::filesystem::path StateRoot;
	std::vector<RecentProject> RecentProjects;
};
} // namespace editor::project
