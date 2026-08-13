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
[[nodiscard]] string NativeError(const string_view Prefix, const DWORD Code)
{
	return string(Prefix) + ": " + std::system_category().message(static_cast<int32>(Code));
}
} // namespace

uint32 StandaloneGameLauncher::Launch(const StandaloneGameLaunchSpecification &Specification)
{
	if (!std::filesystem::is_regular_file(Specification.PackagedExecutable))
		throw StandaloneGameLaunchException("Packaged game executable does not exist: '" + Specification.PackagedExecutable.string() + "'");
	std::wstring MutableText = L"\"" + Specification.PackagedExecutable.native() + L"\"";
	std::vector<wchar_t> MutableCommandLine(MutableText.begin(), MutableText.end());
	MutableCommandLine.push_back(L'\0');
	STARTUPINFOW Startup{};
	Startup.cb = sizeof(Startup);
	PROCESS_INFORMATION Process{};
	const std::wstring WorkingDirectory = Specification.PackagedExecutable.parent_path().native();
	if (CreateProcessW(Specification.PackagedExecutable.c_str(), MutableCommandLine.data(), nullptr, nullptr, FALSE,
					   CREATE_NEW_PROCESS_GROUP, nullptr, WorkingDirectory.c_str(), &Startup, &Process) == FALSE)
	{
		throw StandaloneGameLaunchException(NativeError("Could not launch standalone game", GetLastError()));
	}
	CloseHandle(Process.hThread);
	CloseHandle(Process.hProcess);
	return static_cast<uint32>(Process.dwProcessId);
}
} // namespace editor::play
