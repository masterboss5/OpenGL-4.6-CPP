#pragma once

#include "src/resource/asset/AssetManager.h"
#include "src/runtime/project/ProjectDescriptor.h"
#include "src/runtime/project/VirtualFileSystem.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace editor::project
{
using ProjectDescriptor = runtime::project::ProjectDescriptor;

struct ProjectPaths final
{
	std::filesystem::path Root;
	std::filesystem::path Content;
	std::filesystem::path Source;
	std::filesystem::path Config;
	std::filesystem::path Saved;
	std::filesystem::path Autosaves;
	std::filesystem::path Recovery;
	std::filesystem::path Trash;
	std::filesystem::path Logs;
	std::filesystem::path Layouts;
	std::filesystem::path Intermediate;
	std::filesystem::path AssetRegistry;
	std::filesystem::path Cook;
	std::filesystem::path HotReload;
	std::filesystem::path IntermediateBuild;
	std::filesystem::path Build;
	std::filesystem::path DevelopmentBuild;
	std::filesystem::path ShippingBuild;

	[[nodiscard]] static ProjectPaths FromDescriptor(const std::filesystem::path &DescriptorPath);
	[[nodiscard]] bool Contains(const std::filesystem::path &Path) const;
};

class Project final
{
  public:
	explicit Project(ProjectDescriptor Descriptor);

	Project(const Project &) = delete;
	Project &operator=(const Project &) = delete;
	Project(Project &&) = delete;
	Project &operator=(Project &&) = delete;

	void ValidateLayout() const;
	void CreateMissingDirectories() const;
	[[nodiscard]] std::filesystem::path ResolveContentPath(const std::filesystem::path &RelativePath) const;
	[[nodiscard]] std::filesystem::path ResolveProjectPath(const std::filesystem::path &RelativePath) const;
	[[nodiscard]] std::filesystem::path MakeContentRelative(const std::filesystem::path &AbsolutePath) const;
	[[nodiscard]] std::filesystem::path ResolveVirtualPath(string_view VirtualPath) const;
	[[nodiscard]] string MakeVirtualPath(const std::filesystem::path &AbsolutePath) const;
	[[nodiscard]] const ProjectDescriptor &GetDescriptor() const noexcept;
	[[nodiscard]] const ProjectPaths &GetPaths() const noexcept;
	[[nodiscard]] const runtime::project::VirtualFileSystem &GetVirtualFileSystem() const noexcept;
	[[nodiscard]] resource::AssetManager &GetAssetManager() noexcept;
	[[nodiscard]] const resource::AssetManager &GetAssetManager() const noexcept;

  private:
	ProjectDescriptor Descriptor;
	ProjectPaths Paths;
	runtime::project::VirtualFileSystem VirtualFileSystem;
	std::unique_ptr<resource::AssetManager> AssetManager;
};

class ProjectManager final
{
  public:
	explicit ProjectManager(ProjectDescriptor Descriptor);
	~ProjectManager() = default;

	ProjectManager(const ProjectManager &) = delete;
	ProjectManager &operator=(const ProjectManager &) = delete;
	ProjectManager(ProjectManager &&) = delete;
	ProjectManager &operator=(ProjectManager &&) = delete;

	void Open(ProjectDescriptor Descriptor);
	void Close() noexcept;
	[[nodiscard]] bool IsOpen() const noexcept;
	[[nodiscard]] Project &GetProject();
	[[nodiscard]] const Project &GetProject() const;

  private:
	std::unique_ptr<Project> Current;
};
} // namespace editor::project
