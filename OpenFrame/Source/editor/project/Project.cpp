#include "Project.h"

#include "Source/core/io/SecurePath.h"
#include "Source/core/io/UserPaths.h"

#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
[[nodiscard]] bool IsPathWithin(const std::filesystem::path &Root, const std::filesystem::path &Candidate)
{
	try
	{
		core::io::SecurePath::VerifyContained(Root, Candidate, "Project path");
		return true;
	}
	catch (const core::io::SecurePathException &)
	{
		return false;
	}
}
} // namespace

namespace editor::project
{
ProjectPaths ProjectPaths::FromDescriptor(const std::filesystem::path &DescriptorPath, const util::UUID &ProjectID)
{
	if (DescriptorPath.empty())
		throw std::invalid_argument("Project descriptor path cannot be empty");
	if (DescriptorPath.extension() != ".engineproject")
		throw std::invalid_argument("Project descriptor must use the .engineproject extension");
	if (!ProjectID.IsValid())
		throw std::invalid_argument("Project paths require a valid project identity");

	const std::filesystem::path Root = std::filesystem::absolute(DescriptorPath).lexically_normal().parent_path();
	const std::filesystem::path GeneratedRoot = core::io::UserPaths::OpenFrameApplicationData() / "Projects" / ProjectID.ToString();
	return {.Root = Root,
			.Content = Root / "Content",
			.Source = Root / "Source",
			.Config = Root / "Config",
			.Saved = GeneratedRoot / "Saved",
			.Autosaves = GeneratedRoot / "Saved" / "Autosaves",
			.Recovery = GeneratedRoot / "Saved" / "Recovery",
			.Trash = GeneratedRoot / "Saved" / "Trash",
			.Logs = GeneratedRoot / "Saved" / "Logs",
			.Layouts = GeneratedRoot / "Saved" / "Layouts",
			.Intermediate = GeneratedRoot / "Intermediate",
			.AssetRegistry = GeneratedRoot / "Intermediate" / "AssetRegistry",
			.Cook = GeneratedRoot / "Intermediate" / "Cook",
			.HotReload = GeneratedRoot / "Intermediate" / "HotReload",
			.IntermediateBuild = GeneratedRoot / "Intermediate" / "Build",
			.Build = GeneratedRoot / "Build",
			.DevelopmentBuild = GeneratedRoot / "Build" / "Development",
			.ShippingBuild = GeneratedRoot / "Build" / "Shipping"};
}

bool ProjectPaths::Contains(const std::filesystem::path &Path) const
{
	return IsPathWithin(this->Root, Path);
}

Project::Project(ProjectDescriptor Descriptor)
	: Descriptor(std::move(Descriptor)), Paths(ProjectPaths::FromDescriptor(this->Descriptor.DescriptorPath, this->Descriptor.ID)),
	  VirtualFileSystem(this->Paths.Root, this->Descriptor.ContentMounts),
	  AssetManager(std::make_unique<resource::AssetManager>(this->Paths.Content))
{
	if (!this->Descriptor.ID.IsValid())
		throw std::invalid_argument("Project identity must be valid");
	if (this->Descriptor.Name.empty())
		throw std::invalid_argument("Project name cannot be empty");
	if (this->Descriptor.FormatVersion == 0)
		throw std::invalid_argument("Project format version must be non-zero");
	if (!this->Descriptor.StartupScene.empty() && this->Descriptor.StartupScene.is_absolute())
		throw std::invalid_argument("Project startup scene must be relative to the Content directory");
	if (!this->Descriptor.GameModule.empty() && this->Descriptor.GameModule.is_absolute())
		throw std::invalid_argument("Project game module must be relative to the project root");
}

void Project::ValidateLayout() const
{
	std::error_code Error;
	if (!std::filesystem::exists(this->Paths.Root, Error) || Error)
		throw std::runtime_error("Project root directory does not exist");
	if (!std::filesystem::exists(this->Descriptor.DescriptorPath, Error) || Error)
		throw std::runtime_error("Project descriptor does not exist");
	if (!std::filesystem::is_regular_file(this->Descriptor.DescriptorPath, Error) || Error)
		throw std::runtime_error("Project descriptor path is not a regular file");
	if (!this->Descriptor.StartupScene.empty())
	{
		const std::filesystem::path StartupScene = this->ResolveContentPath(this->Descriptor.StartupScene);
		if (!std::filesystem::exists(StartupScene, Error) || Error)
			throw std::runtime_error("Configured project startup scene does not exist");
	}
}

void Project::CreateMissingDirectories() const
{
	core::io::SecurePath::CreateTrustedRoot(this->Paths.Saved.parent_path(), "OpenFrame project data root");
	const std::filesystem::path Directories[] = {this->Paths.Content,
												 this->Paths.Source,
												 this->Paths.Config,
												 this->Paths.Autosaves,
												 this->Paths.Recovery,
												 this->Paths.Trash,
												 this->Paths.Logs,
												 this->Paths.Layouts,
												 this->Paths.Intermediate,
												 this->Paths.AssetRegistry,
												 this->Paths.Cook,
												 this->Paths.HotReload,
												 this->Paths.IntermediateBuild,
												 this->Paths.Build,
												 this->Paths.DevelopmentBuild,
												 this->Paths.ShippingBuild};
	for (const std::filesystem::path &Directory : Directories)
	{
		const bool Authored = Directory == this->Paths.Content || Directory == this->Paths.Source || Directory == this->Paths.Config;
		const std::filesystem::path &TrustedRoot = Authored ? this->Paths.Root : this->Paths.Saved.parent_path();
		core::io::SecurePath::CreateDirectoriesWithin(TrustedRoot, Directory.lexically_relative(TrustedRoot), "Project directory");
	}
	for (const runtime::project::ResolvedContentMount &Mount : this->VirtualFileSystem.GetMounts())
	{
		const std::filesystem::path Relative = Mount.PhysicalRoot.lexically_relative(this->Paths.Root);
		if (!Relative.empty() && !Relative.is_absolute() && *Relative.begin() != "..")
			core::io::SecurePath::CreateDirectoriesWithin(this->Paths.Root, Relative, "Content mount");
		else
		{
			const std::filesystem::path Parent = Mount.PhysicalRoot.parent_path();
			core::io::SecurePath::CreateDirectoriesWithin(Parent, Mount.PhysicalRoot.filename(), "External content mount");
		}
	}
}

std::filesystem::path Project::ResolveContentPath(const std::filesystem::path &RelativePath) const
{
	if (RelativePath.empty() || RelativePath.is_absolute())
		throw std::invalid_argument("Content path must be a non-empty relative path");
	return core::io::SecurePath::ResolveWithin(this->Paths.Content, RelativePath, "Content path");
}

std::filesystem::path Project::ResolveProjectPath(const std::filesystem::path &RelativePath) const
{
	if (RelativePath.empty() || RelativePath.is_absolute())
		throw std::invalid_argument("Project path must be a non-empty relative path");
	return core::io::SecurePath::ResolveWithin(this->Paths.Root, RelativePath, "Project path");
}

std::filesystem::path Project::MakeContentRelative(const std::filesystem::path &AbsolutePath) const
{
	if (!IsPathWithin(this->Paths.Content, AbsolutePath))
		throw std::invalid_argument("Path is outside the project Content directory");
	return std::filesystem::absolute(AbsolutePath).lexically_normal().lexically_relative(this->Paths.Content);
}

std::filesystem::path Project::ResolveVirtualPath(const string_view VirtualPath) const
{
	return this->VirtualFileSystem.Resolve(VirtualPath);
}

string Project::MakeVirtualPath(const std::filesystem::path &AbsolutePath) const
{
	return this->VirtualFileSystem.MakeVirtual(AbsolutePath);
}

const ProjectDescriptor &Project::GetDescriptor() const noexcept
{
	return this->Descriptor;
}

const ProjectPaths &Project::GetPaths() const noexcept
{
	return this->Paths;
}

const runtime::project::VirtualFileSystem &Project::GetVirtualFileSystem() const noexcept
{
	return this->VirtualFileSystem;
}

resource::AssetManager &Project::GetAssetManager() noexcept
{
	return *this->AssetManager;
}

const resource::AssetManager &Project::GetAssetManager() const noexcept
{
	return *this->AssetManager;
}

ProjectManager::ProjectManager(ProjectDescriptor Descriptor)
{
	this->Open(std::move(Descriptor));
}

void ProjectManager::Open(ProjectDescriptor Descriptor)
{
	if (this->Current != nullptr)
		throw std::logic_error("Project manager already owns an open project");
	this->Current = std::make_unique<Project>(std::move(Descriptor));
}

void ProjectManager::Close() noexcept
{
	this->Current.reset();
}

bool ProjectManager::IsOpen() const noexcept
{
	return this->Current != nullptr;
}

Project &ProjectManager::GetProject()
{
	if (this->Current == nullptr)
		throw std::logic_error("Project manager has no open project");
	return *this->Current;
}

const Project &ProjectManager::GetProject() const
{
	if (this->Current == nullptr)
		throw std::logic_error("Project manager has no open project");
	return *this->Current;
}
} // namespace editor::project
