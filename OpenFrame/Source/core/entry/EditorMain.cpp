#include "Source/core/app/Application.h"
#include "Source/core/layers/EditorLayer.h"
#include "Source/editor/serialization/ProjectDescriptorSerializer.h"
#include "Source/runtime/project/ProjectPackage.h"
#include "Source/types.h"
#include "Source/util/logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace
{
[[nodiscard]] std::wstring ToWide(const string_view Text)
{
	if (Text.empty())
		return {};
	const int32 Required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int32>(Text.size()), nullptr, 0);
	if (Required <= 0)
		return L"OpenFrame encountered an error whose message could not be decoded.";
	std::wstring Result(static_cast<usize>(Required), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int32>(Text.size()), Result.data(), Required) !=
		Required)
		return L"OpenFrame encountered an error whose message could not be decoded.";
	return Result;
}

void ShowFatalError(const string_view Message) noexcept
{
	try
	{
		const std::wstring Wide = ToWide(Message);
		MessageBoxW(nullptr, Wide.c_str(), L"OpenFrame Editor", MB_OK | MB_ICONERROR | MB_TASKMODAL);
	}
	catch (...)
	{
		MessageBoxW(nullptr, L"OpenFrame encountered a fatal error.", L"OpenFrame Editor", MB_OK | MB_ICONERROR | MB_TASKMODAL);
	}
}

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
		Candidates.push_back(Ancestor / "OpenFrame");
		Candidates.push_back(Ancestor);
		Ancestor = Ancestor.parent_path();
	}
	for (const std::filesystem::path &Candidate : Candidates)
	{
		std::error_code Error;
		const std::filesystem::path Normal = std::filesystem::absolute(Candidate).lexically_normal();
		if (std::filesystem::is_directory(Normal / "Shaders", Error) && !Error)
			return Normal;
	}
	throw std::runtime_error("Could not locate OpenFrame content containing the Shaders directory");
}

[[nodiscard]] std::vector<editor::cook::RuntimePackageFile> BuildRuntimePackageFiles(const editor::project::ProjectDescriptor &Project,
																					 const std::filesystem::path &Executable,
																					 const std::filesystem::path &EngineContent)
{
	const std::filesystem::path RuntimeDirectory = Executable.parent_path();
	const std::filesystem::path GameExecutable = RuntimeDirectory / "OpenFrameGame.exe";
	const std::filesystem::path EngineLibrary = RuntimeDirectory / "OpenFrameEngine.dll";
	if (!std::filesystem::is_regular_file(EngineLibrary))
		throw std::runtime_error("Editor packaging requires OpenFrameEngine.dll beside OpenFrameEditor.exe");
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
			throw std::runtime_error("Editor packaging requires " + Library.filename().string() + " beside OpenFrameEditor.exe");
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
			 Iterator(EngineContent / "Shaders", std::filesystem::directory_options::skip_permission_denied, Error),
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t *, int)
{
	try
	{
		LOG_INFO("[Starting Editor]");
		const std::filesystem::path Executable = GetExecutablePath();
		const std::filesystem::path EngineContent = FindEngineContentRoot(Executable);
		core::WindowSpecification Window;
		Window.Title = "OpenFrame";
		Window.Extent = {1600, 900};
		Window.Maximized = true;
		core::Application Application({.Window = std::move(Window)});
		Application.PushLayer<editor::EditorLayer>(editor::EditorLayerSpecification{
			.EngineContentRoot = EngineContent,
			.ConfigureProject = [Executable, EngineContent](const editor::project::ProjectDescriptor &Project)
			{
				editor::EditorLayerSpecification::ProjectConfiguration Configuration;
				Configuration.RuntimePackageFiles = BuildRuntimePackageFiles(Project, Executable, EngineContent);
				return Configuration;
			}});
		Application.Main();
		return EXIT_SUCCESS;
	}
	catch (const std::exception &Exception)
	{
		ShowFatalError(Exception.what());
		return EXIT_FAILURE;
	}
	catch (...)
	{
		ShowFatalError("Unknown non-standard exception");
		return EXIT_FAILURE;
	}
}
