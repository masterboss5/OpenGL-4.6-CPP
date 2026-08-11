#include "src/core/app/Application.h"
#include "src/core/layers/GameLayer.h"
#include "src/runtime/project/ProjectDescriptor.h"
#include "src/runtime/project/ProjectPackage.h"
#include "src/types.h"
#include "src/util/logger.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
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
		throw std::runtime_error("Could not resolve the Game executable path");
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

[[nodiscard]] core::GameLayerSpecification LoadProject(const std::filesystem::path &DescriptorPath, const std::filesystem::path &Executable,
													   const std::optional<std::filesystem::path> &SceneOverride)
{
	const runtime::project::ProjectDescriptor Descriptor = runtime::project::ProjectDescriptorLoader::Load(DescriptorPath);
	const std::filesystem::path Root = Descriptor.DescriptorPath.parent_path();
	return {.ProjectName = Descriptor.Name,
			.ContentRoot = Root / "Content",
			.EngineContentRoot = FindEngineContentRoot(Executable),
			.StartupScene = SceneOverride.value_or(Descriptor.StartupScene),
			.GameModule = Descriptor.GameModule.empty() ? std::filesystem::path{} : Root / Descriptor.GameModule,
			.CacheRoot = Root / "Intermediate"};
}
} // namespace

int wmain(const int ArgumentCount, wchar_t *Arguments[])
{
	try
	{
		LOG_INFO("[Starting Game]");
		const std::filesystem::path Executable = GetExecutablePath();
		std::optional<std::filesystem::path> DescriptorPath;
		std::optional<std::filesystem::path> SceneOverride;
		for (int32 ArgumentIndex = 1; ArgumentIndex < ArgumentCount; ++ArgumentIndex)
		{
			const std::wstring_view Argument = Arguments[ArgumentIndex];
			if (Argument == L"--game")
			{
				if (ArgumentIndex + 1 >= ArgumentCount)
					throw std::invalid_argument("--game requires an .engineproject path");
				DescriptorPath = std::filesystem::path(Arguments[++ArgumentIndex]);
			}
			else if (Argument == L"--scene")
			{
				if (ArgumentIndex + 1 >= ArgumentCount)
					throw std::invalid_argument("--scene requires a Content-relative scene path");
				SceneOverride = std::filesystem::path(Arguments[++ArgumentIndex]);
				if (SceneOverride->empty() || SceneOverride->is_absolute())
					throw std::invalid_argument("--scene must be a non-empty Content-relative path");
			}
			else if (!DescriptorPath.has_value())
				DescriptorPath = std::filesystem::path(Argument);
			else
				throw std::invalid_argument("Game accepts one .engineproject path");
		}

		core::GameLayerSpecification Game;
		if (DescriptorPath.has_value())
			Game = LoadProject(*DescriptorPath, Executable, SceneOverride);
		else if (runtime::project::ProjectPackage::IsPackage(Executable.parent_path()))
		{
			if (SceneOverride.has_value())
				throw std::invalid_argument("Packaged games do not accept a loose scene override");
			const runtime::project::ProjectPackageMount Mount = runtime::project::ProjectPackage::Mount(Executable.parent_path());
			Game = {.ProjectName = Mount.ProjectName,
					.ContentRoot = Mount.ContentRoot,
					.EngineContentRoot = Mount.EngineContentRoot,
					.StartupScene = Mount.StartupScene,
					.GameModule = Mount.GameModule,
					.CacheRoot = Mount.ContentRoot.parent_path()};
		}
		else
			throw std::invalid_argument("No project was selected. Launch Game.exe <project.engineproject>.");

		core::WindowSpecification Window;
		Window.Title = Game.ProjectName;
		Window.Extent = {1920, 1080};
		core::Application Application({.Window = std::move(Window)});
		Application.PushLayer<core::GameLayer>(std::move(Game));
		Application.Main();
		return EXIT_SUCCESS;
	}
	catch (const std::exception &Exception)
	{
		std::cerr << "Fatal game exception: " << Exception.what() << '\n';
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::cerr << "Fatal game exception: unknown non-standard exception\n";
		return EXIT_FAILURE;
	}
}
