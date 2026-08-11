#include "src/core/app/Application.h"
#include "src/core/layers/EditorLayer.h"
#include "src/editor/serialization/ProjectDescriptorSerializer.h"
#include "src/runtime/project/ProjectPackage.h"
#include "src/types.h"
#include "src/util/logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace
{
[[nodiscard]] std::filesystem::path GetExecutablePath()
{
	std::vector<wchar_t> Buffer(32'768);
	const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
	if (Length == 0 || Length >= Buffer.size())
		throw std::runtime_error("Could not resolve the Editor executable path");
	return std::filesystem::path(Buffer.data(), Buffer.data() + Length);
}

[[nodiscard]] std::filesystem::path FindEngineContentRoot(const std::filesystem::path &Executable)
{
	std::vector<std::filesystem::path> Candidates{std::filesystem::current_path(), Executable.parent_path()};
	std::filesystem::path Ancestor = Executable.parent_path();
	for (uint32 Depth = 0; Depth < 5 && !Ancestor.empty(); ++Depth)
	{
		Candidates.push_back(Ancestor / "OpenGL");
		Candidates.push_back(Ancestor);
		Ancestor = Ancestor.parent_path();
	}
	for (const std::filesystem::path &Candidate : Candidates)
	{
		std::error_code Error;
		const std::filesystem::path Normal = std::filesystem::absolute(Candidate).lexically_normal();
		if (std::filesystem::is_directory(Normal / "shader", Error) && !Error)
			return Normal;
	}
	throw std::runtime_error("Could not locate engine content containing the shader directory");
}

[[nodiscard]] std::filesystem::path FindSolution(const std::filesystem::path &Executable)
{
	std::filesystem::path Ancestor = Executable.parent_path();
	for (uint32 Depth = 0; Depth < 8 && !Ancestor.empty(); ++Depth)
	{
		const std::filesystem::path Candidate = Ancestor / "OpenGL.sln";
		if (std::filesystem::is_regular_file(Candidate))
			return std::filesystem::absolute(Candidate).lexically_normal();
		Ancestor = Ancestor.parent_path();
	}
	throw std::runtime_error("Could not locate OpenGL.sln for GameModule builds");
}

[[nodiscard]] std::filesystem::path FindMSBuild()
{
	std::array<wchar_t, 32'768> ProgramFiles{};
	const DWORD Length = GetEnvironmentVariableW(L"ProgramFiles", ProgramFiles.data(), static_cast<DWORD>(ProgramFiles.size()));
	if (Length == 0 || Length >= ProgramFiles.size())
		throw std::runtime_error("Could not resolve the Program Files directory for MSBuild discovery");
	const std::filesystem::path VisualStudioRoot = std::filesystem::path(ProgramFiles.data()) / "Microsoft Visual Studio";
	constexpr std::array Editions{string_view{"Community"}, string_view{"Professional"}, string_view{"Enterprise"},
								  string_view{"BuildTools"}};
	for (const string_view Edition : Editions)
	{
		const std::filesystem::path Candidate = VisualStudioRoot / "2022" / Edition / "MSBuild" / "Current" / "Bin" / "MSBuild.exe";
		if (std::filesystem::is_regular_file(Candidate))
			return Candidate;
	}
	throw std::runtime_error("Could not locate a Visual Studio 2022 MSBuild installation");
}

[[nodiscard]] std::filesystem::path ParseProjectPath(const int32 ArgumentCount, wchar_t *Arguments[])
{
	if (ArgumentCount == 2)
		return Arguments[1];
	if (ArgumentCount == 3 && std::wstring_view(Arguments[1]) == L"--editor")
		return Arguments[2];
	throw std::invalid_argument("Editor requires exactly one .engineproject path");
}

[[nodiscard]] std::vector<editor::cook::RuntimePackageFile> BuildRuntimePackageFiles(const editor::project::ProjectDescriptor &Project,
																					 const std::filesystem::path &Executable,
																					 const std::filesystem::path &EngineContent)
{
	const std::filesystem::path RuntimeDirectory = Executable.parent_path();
	const std::filesystem::path GameExecutable = RuntimeDirectory / "Game.exe";
	const std::filesystem::path EngineLibrary = RuntimeDirectory / "Engine.dll";
	if (!std::filesystem::is_regular_file(EngineLibrary))
		throw std::runtime_error("Editor packaging requires Engine.dll beside Editor.exe");
	// Keep the expected Game path in the package manifest even before the first
	// project build.  Standalone launch and packaging validate the source at the
	// operation boundary, while the editor itself remains usable for authoring.

	std::vector<editor::cook::RuntimePackageFile> Files{
		{.Source = GameExecutable, .Destination = Project.Name + ".exe", .Kind = runtime::project::PackageFileKind::Executable},
		{.Source = EngineLibrary, .Destination = EngineLibrary.filename(), .Kind = runtime::project::PackageFileKind::DynamicLibrary}};
	const std::array RequiredLibraries{RuntimeDirectory / "assimp-vc145-mt.dll", RuntimeDirectory / "glew32.dll"};
	for (const std::filesystem::path &Library : RequiredLibraries)
	{
		if (!std::filesystem::is_regular_file(Library))
			throw std::runtime_error("Editor packaging requires " + Library.filename().string() + " beside Editor.exe");
		Files.push_back({.Source = Library, .Destination = Library.filename(), .Kind = runtime::project::PackageFileKind::DynamicLibrary});
	}

	if (!Project.GameModule.empty())
	{
		const std::filesystem::path GameModule =
			std::filesystem::absolute(Project.DescriptorPath.parent_path() / Project.GameModule).lexically_normal();
		Files.push_back({.Source = GameModule,
						 .Destination = std::filesystem::path("GameModules") / GameModule.filename(),
						 .Kind = runtime::project::PackageFileKind::GameModule});
		std::error_code Error;
		if (std::filesystem::is_directory(GameModule.parent_path(), Error) && !Error)
		{
			for (std::filesystem::directory_iterator
					 Iterator(GameModule.parent_path(), std::filesystem::directory_options::skip_permission_denied, Error),
				 End;
				 Iterator != End; Iterator.increment(Error))
			{
				if (Error)
					throw std::runtime_error("Could not enumerate GameModule dependencies: " + Error.message());
				if (!Iterator->is_regular_file() || Iterator->path() == GameModule)
					continue;
				string Extension = Iterator->path().extension().string();
				std::ranges::transform(Extension, Extension.begin(), [](const char Character)
									   { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
				if (Extension == ".dll")
				{
					Files.push_back({.Source = Iterator->path(),
									 .Destination = std::filesystem::path("GameModules") / Iterator->path().filename(),
									 .Kind = runtime::project::PackageFileKind::DynamicLibrary});
				}
			}
			if (Error)
			{
				std::error_code ExistsError;
				if (std::filesystem::exists(GameModule.parent_path(), ExistsError) || ExistsError)
					throw std::runtime_error("Could not enumerate GameModule dependencies: " + Error.message());
			}
		}
		else if (Error)
		{
			std::error_code ExistsError;
			if (std::filesystem::exists(GameModule.parent_path(), ExistsError) || ExistsError)
				throw std::runtime_error("Could not inspect the GameModule output directory: " + Error.message());
		}
	}

	std::error_code Error;
	for (std::filesystem::recursive_directory_iterator
			 Iterator(EngineContent / "shader", std::filesystem::directory_options::skip_permission_denied, Error),
		 End;
		 Iterator != End; Iterator.increment(Error))
	{
		if (Error)
			throw std::runtime_error("Could not enumerate engine shader content: " + Error.message());
		if (!Iterator->is_regular_file())
			continue;
		const std::filesystem::path Relative = Iterator->path().lexically_relative(EngineContent);
		Files.push_back({.Source = Iterator->path(),
						 .Destination = std::filesystem::path("Engine") / Relative,
						 .Kind = runtime::project::PackageFileKind::EngineContent});
	}
	if (Error)
		throw std::runtime_error("Could not enumerate engine shader content: " + Error.message());
	return Files;
}
} // namespace

int wmain(const int ArgumentCount, wchar_t *Arguments[])
{
	try
	{
		LOG_INFO("[Starting Editor]");
		const std::filesystem::path ProjectPath = ParseProjectPath(ArgumentCount, Arguments);
		const editor::project::ProjectDescriptor Project = editor::serialization::ProjectDescriptorSerializer::Load(ProjectPath);
		const std::filesystem::path Executable = GetExecutablePath();
		const std::filesystem::path EngineContent = FindEngineContentRoot(Executable);
		constexpr string_view BuildConfiguration =
#ifdef _DEBUG
			"Debug";
#else
			"Release";
#endif
		const std::filesystem::path PublishedGameModule =
			Project.GameModule.empty()
				? std::filesystem::path{}
				: std::filesystem::absolute(Project.DescriptorPath.parent_path() / Project.GameModule).lexically_normal();
		const editor::build::GameModuleBuildSpecification GameModuleBuild{
			.MSBuildExecutable = FindMSBuild(),
			.Solution = FindSolution(Executable),
			.Target = "GameModule",
			.Configuration = string(BuildConfiguration),
			.Platform = "x64",
			.BuiltModule = Project.GameModule.empty() ? std::filesystem::path{} : Executable.parent_path() / PublishedGameModule.filename(),
			.PublishedModule = PublishedGameModule,
			.PackageOutputRoot = Project.DescriptorPath.parent_path() / "Intermediate" / "PackageBuild" / string(BuildConfiguration),
			.EngineContentRoot = EngineContent};
		core::WindowSpecification Window;
		Window.Title = Project.Name + " Editor";
		Window.Extent = {1600, 900};
		Window.Maximized = true;
		core::Application Application({.Window = std::move(Window)});
		editor::EditorLayer &Layer = Application.PushLayer<editor::EditorLayer>(
			editor::EditorLayerSpecification{.Project = Project,
											 .EngineContentRoot = EngineContent,
											 .RuntimePackageFiles = BuildRuntimePackageFiles(Project, Executable, EngineContent),
											 .GameModuleBuild = GameModuleBuild});
		if (!Project.StartupScene.empty())
			Layer.GetSession().OpenDocument(Layer.GetSession().GetProject().ResolveContentPath(Project.StartupScene));
		Application.Main();
		return EXIT_SUCCESS;
	}
	catch (const std::exception &Exception)
	{
		std::cerr << "Fatal Editor exception: " << Exception.what() << '\n';
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::cerr << "Fatal Editor exception: unknown non-standard exception\n";
		return EXIT_FAILURE;
	}
}
