#include "VirtualFileSystem.h"

#include "Source/core/io/SecurePath.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace runtime::project
{
namespace
{
[[nodiscard]] string Lowercase(string Value)
{
	std::ranges::transform(Value, Value.begin(),
						   [](const char Character) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
	return Value;
}

[[nodiscard]] string NormalizeVirtualRoot(string Value)
{
	std::ranges::replace(Value, '\\', '/');
	while (Value.size() > 1 && Value.back() == '/')
		Value.pop_back();
	if (Value.empty() || Value.front() != '/' || Value == "/")
		throw VirtualFileSystemException("Content mount virtual roots must be named absolute roots such as /Game");
	if (Value.find("/../") != string::npos || Value.ends_with("/..") || Value.find("/./") != string::npos || Value.ends_with("/."))
		throw VirtualFileSystemException("Content mount virtual roots cannot contain traversal components");
	return Value;
}

[[nodiscard]] bool MatchesVirtualRoot(const string_view Path, const string_view Root)
{
	if (Path.size() < Root.size())
		return false;
	if (!std::ranges::equal(Path.substr(0, Root.size()), Root, [](const char Left, const char Right)
							{ return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right)); }))
	{
		return false;
	}
	return Path.size() == Root.size() || Path[Root.size()] == '/';
}
} // namespace

VirtualFileSystem::VirtualFileSystem(std::filesystem::path ProjectRoot, const std::span<const ProjectContentMount> Mounts)
	: ProjectRoot(std::filesystem::absolute(std::move(ProjectRoot)).lexically_normal())
{
	if (Mounts.empty())
		throw VirtualFileSystemException("Virtual file system requires at least one content mount");
	std::unordered_set<string> UniqueRoots;
	this->Mounts.reserve(Mounts.size());
	for (const ProjectContentMount &Mount : Mounts)
	{
		const string VirtualRoot = NormalizeVirtualRoot(Mount.VirtualRoot);
		if (!UniqueRoots.emplace(Lowercase(VirtualRoot)).second)
			throw VirtualFileSystemException("Duplicate content mount virtual root: " + VirtualRoot);
		if (Mount.PhysicalRoot.empty() || Mount.PhysicalRoot.is_absolute())
			throw VirtualFileSystemException("Content mount physical roots must be project-relative");
		std::filesystem::path PhysicalRoot;
		try
		{
			PhysicalRoot = core::io::SecurePath::ResolveWithin(this->ProjectRoot, Mount.PhysicalRoot, "Content mount physical root");
		}
		catch (const core::io::SecurePathException &Exception)
		{
			throw VirtualFileSystemException(Exception.what());
		}
		this->Mounts.push_back({.VirtualRoot = VirtualRoot, .PhysicalRoot = PhysicalRoot, .ReadOnly = Mount.ReadOnly});
	}
	std::ranges::sort(this->Mounts, [](const ResolvedContentMount &Left, const ResolvedContentMount &Right)
					  { return Left.VirtualRoot.size() > Right.VirtualRoot.size(); });
}

std::filesystem::path VirtualFileSystem::Resolve(const string_view VirtualPath) const
{
	string Normal(VirtualPath);
	std::ranges::replace(Normal, '\\', '/');
	if (Normal.empty() || Normal.front() != '/')
		throw VirtualFileSystemException("Virtual paths must begin with '/'");
	const ResolvedContentMount &Mount = this->FindMount(Normal);
	string RelativeText = Normal.substr(Mount.VirtualRoot.size());
	while (!RelativeText.empty() && RelativeText.front() == '/')
		RelativeText.erase(RelativeText.begin());
	const std::filesystem::path Relative = std::filesystem::path(RelativeText).lexically_normal();
	if (Relative.is_absolute() || Relative.has_root_name())
		throw VirtualFileSystemException("Virtual path contains an invalid rooted suffix");
	try
	{
		if (Relative.empty() || Relative == ".")
		{
			core::io::SecurePath::VerifyContained(Mount.PhysicalRoot, Mount.PhysicalRoot, "Virtual path");
			return Mount.PhysicalRoot;
		}
		return core::io::SecurePath::ResolveWithin(Mount.PhysicalRoot, Relative, "Virtual path");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw VirtualFileSystemException(Exception.what());
	}
}

string VirtualFileSystem::MakeVirtual(const std::filesystem::path &PhysicalPath) const
{
	const std::filesystem::path Normal = std::filesystem::absolute(PhysicalPath).lexically_normal();
	for (const ResolvedContentMount &Mount : this->Mounts)
	{
		try
		{
			core::io::SecurePath::VerifyContained(Mount.PhysicalRoot, Normal, "Physical content path");
		}
		catch (const core::io::SecurePathException &)
		{
			continue;
		}
		const std::filesystem::path Relative = Normal.lexically_relative(Mount.PhysicalRoot);
		return Relative.empty() || Relative == "." ? Mount.VirtualRoot : Mount.VirtualRoot + "/" + Relative.generic_string();
	}
	throw VirtualFileSystemException("Physical path does not belong to a configured content mount");
}

const ResolvedContentMount &VirtualFileSystem::FindMount(const string_view VirtualPath) const
{
	const auto Mount = std::ranges::find_if(this->Mounts, [VirtualPath](const ResolvedContentMount &Candidate)
											{ return MatchesVirtualRoot(VirtualPath, Candidate.VirtualRoot); });
	if (Mount == this->Mounts.end())
		throw VirtualFileSystemException("Virtual path does not belong to a configured content mount: " + string(VirtualPath));
	return *Mount;
}

std::span<const ResolvedContentMount> VirtualFileSystem::GetMounts() const noexcept
{
	return this->Mounts;
}
} // namespace runtime::project
