#include "UserPaths.h"

#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>

namespace core::io
{
namespace
{
[[nodiscard]] std::filesystem::path GetKnownFolder(const KNOWNFOLDERID &Identity, const char *Name)
{
	wchar_t *RawPath = nullptr;
	const HRESULT Result = SHGetKnownFolderPath(Identity, KF_FLAG_CREATE, nullptr, &RawPath);
	if (FAILED(Result) || RawPath == nullptr)
		throw std::runtime_error(std::string("Could not resolve the Windows ") + Name + " folder");
	const std::filesystem::path Path(RawPath);
	CoTaskMemFree(RawPath);
	return std::filesystem::absolute(Path).lexically_normal();
}
} // namespace

std::filesystem::path UserPaths::Documents()
{
	return GetKnownFolder(FOLDERID_Documents, "Documents");
}

std::filesystem::path UserPaths::LocalApplicationData()
{
	return GetKnownFolder(FOLDERID_LocalAppData, "Local AppData");
}

std::filesystem::path UserPaths::OpenFrameApplicationData()
{
	return LocalApplicationData() / "OpenFrame";
}

std::filesystem::path UserPaths::OpenFrameProjects()
{
	return Documents() / "OpenFrame Projects";
}
} // namespace core::io
