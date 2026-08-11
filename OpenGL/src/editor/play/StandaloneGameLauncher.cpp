#include "StandaloneGameLauncher.h"

#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace editor::play
{
namespace
{
[[nodiscard]] std::wstring QuoteArgument(const std::filesystem::path &Argument)
{
	const std::wstring Value = Argument.native();
	std::wstring Result(1, L'"');
	usize Backslashes = 0;
	for (const wchar_t Character : Value)
	{
		if (Character == L'\\')
		{
			++Backslashes;
			continue;
		}
		if (Character == L'"')
		{
			Result.append(Backslashes * 2U + 1U, L'\\');
			Result.push_back(Character);
			Backslashes = 0;
			continue;
		}
		Result.append(Backslashes, L'\\');
		Backslashes = 0;
		Result.push_back(Character);
	}
	Result.append(Backslashes * 2U, L'\\');
	Result.push_back(L'"');
	return Result;
}

[[nodiscard]] string NativeError(const string_view Prefix, const DWORD Code)
{
	return string(Prefix) + ": " + std::system_category().message(static_cast<int32>(Code));
}
} // namespace

uint32 StandaloneGameLauncher::Launch(const StandaloneGameLaunchSpecification &Specification)
{
	if (!std::filesystem::is_regular_file(Specification.Executable))
		throw StandaloneGameLaunchException("Standalone game executable does not exist: '" + Specification.Executable.string() + "'");
	if (!std::filesystem::is_regular_file(Specification.ProjectDescriptor) ||
		Specification.ProjectDescriptor.extension() != ".engineproject")
	{
		throw StandaloneGameLaunchException("Standalone launch requires a valid project descriptor");
	}
	if (Specification.Scene.empty() || Specification.Scene.is_absolute())
		throw StandaloneGameLaunchException("Standalone launch scene must be relative to project Content");

	std::wstring CommandLine = QuoteArgument(Specification.Executable) + L" --game " + QuoteArgument(Specification.ProjectDescriptor) +
							   L" --scene " + QuoteArgument(Specification.Scene);
	std::vector<wchar_t> MutableCommandLine(CommandLine.begin(), CommandLine.end());
	MutableCommandLine.push_back(L'\0');
	STARTUPINFOW Startup{};
	Startup.cb = sizeof(Startup);
	PROCESS_INFORMATION Process{};
	const std::wstring WorkingDirectory = Specification.Executable.parent_path().native();
	if (CreateProcessW(Specification.Executable.c_str(), MutableCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP,
					   nullptr, WorkingDirectory.c_str(), &Startup, &Process) == FALSE)
	{
		throw StandaloneGameLaunchException(NativeError("Could not launch standalone game", GetLastError()));
	}
	CloseHandle(Process.hThread);
	CloseHandle(Process.hProcess);
	return static_cast<uint32>(Process.dwProcessId);
}
} // namespace editor::play
