#include "Source/core/app/Application.h"
#include "Source/core/layers/GameLayer.h"
#include "Source/runtime/project/ProjectDescriptor.h"
#include "Source/runtime/project/ProjectPackage.h"
#include "Source/types.h"
#include "Source/util/logger.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>
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
		MessageBoxW(nullptr, Wide.c_str(), L"OpenFrame Game", MB_OK | MB_ICONERROR | MB_TASKMODAL);
	}
	catch (...)
	{
		MessageBoxW(nullptr, L"OpenFrame encountered a fatal error.", L"OpenFrame Game", MB_OK | MB_ICONERROR | MB_TASKMODAL);
	}
}

[[nodiscard]] std::filesystem::path GetExecutablePath()
{
	std::vector<wchar_t> Buffer(32'768);
	const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
	if (Length == 0 || Length >= Buffer.size())
		throw std::runtime_error("Could not resolve the Game executable path");
	return std::filesystem::path(Buffer.data(), Buffer.data() + Length);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t *, int)
{
	try
	{
		LOG_INFO("[Starting Game]");
		const std::filesystem::path Executable = GetExecutablePath();
		core::GameLayerSpecification Game;
		if (runtime::project::ProjectPackage::IsPackage(Executable.parent_path()))
		{
			const runtime::project::ProjectPackageMount Mount = runtime::project::ProjectPackage::Mount(Executable.parent_path());
			Game = {.ProjectName = Mount.ProjectName,
					.ContentRoot = Mount.ContentRoot,
					.EngineContentRoot = Mount.EngineContentRoot,
					.StartupScene = Mount.StartupScene,
					.GameModule = Mount.GameModule,
					.CacheRoot = Mount.ContentRoot.parent_path()};
		}
		else
			throw std::invalid_argument("OpenFrameGame must be launched from a packaged project directory");

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
		ShowFatalError(Exception.what());
		return EXIT_FAILURE;
	}
	catch (...)
	{
		ShowFatalError("Unknown non-standard exception");
		return EXIT_FAILURE;
	}
}
