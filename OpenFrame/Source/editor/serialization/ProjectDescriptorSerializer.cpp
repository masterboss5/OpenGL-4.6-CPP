#include "ProjectDescriptorSerializer.h"

#include "Source/core/io/SecurePath.h"
#include "Source/util/UUID.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace editor::serialization
{
namespace
{
using Json = nlohmann::json;

[[nodiscard]] std::filesystem::path ReadRelativePath(const Json &Object, const string_view Name)
{
	const auto Member = Object.find(Name);
	if (Member == Object.end())
		return {};
	if (!Member->is_string())
		throw ProjectDescriptorSerializationException("Project descriptor field '" + string(Name) + "' must be a string");
	const std::filesystem::path Path = Member->get<string>();
	if (Path.is_absolute())
		throw ProjectDescriptorSerializationException("Project descriptor field '" + string(Name) + "' must be project-relative");
	return Path.lexically_normal();
}

[[nodiscard]] std::span<const uint8> BytesOf(const string &Text) noexcept
{
	return {reinterpret_cast<const uint8 *>(Text.data()), Text.size()};
}
} // namespace

project::ProjectDescriptor ProjectDescriptorSerializer::Load(const std::filesystem::path &Path)
{
	if (Path.empty() || !std::filesystem::is_regular_file(Path))
		throw ProjectDescriptorSerializationException("Project descriptor does not exist: '" + Path.string() + "'");
	Json Root;
	try
	{
		constexpr uint64 MaximumDescriptorBytes = 4U * 1024U * 1024U;
		const std::filesystem::path DirectoryRoot = std::filesystem::absolute(Path.parent_path()).lexically_normal();
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(DirectoryRoot, Path.filename(), MaximumDescriptorBytes, "project descriptor");
		Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
	}
	catch (const nlohmann::json::exception &Exception)
	{
		throw ProjectDescriptorSerializationException("Could not parse project descriptor '" + Path.string() + "': " + Exception.what());
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw ProjectDescriptorSerializationException("Could not securely read project descriptor '" + Path.string() +
													  "': " + Exception.what());
	}
	if (!Root.is_object())
		throw ProjectDescriptorSerializationException("Project descriptor root must be an object");
	const uint32 Version = Root.value("FormatVersion", uint32{0});
	if (Version != CurrentFormatVersion)
		throw ProjectDescriptorSerializationException("Project descriptor version " + std::to_string(Version) + " is unsupported");
	if (!Root.contains("ID") || !Root["ID"].is_string())
		throw ProjectDescriptorSerializationException("Project descriptor requires a canonical ID string");
	if (!Root.contains("Name") || !Root["Name"].is_string() || Root["Name"].get_ref<const string &>().empty())
		throw ProjectDescriptorSerializationException("Project descriptor requires a non-empty Name");

	project::ProjectDescriptor Descriptor{.FormatVersion = Version,
										  .ID = util::UUID::Parse(Root["ID"].get_ref<const string &>()),
										  .EngineSchemaVersion = Root.value("EngineSchemaVersion", uint32{1}),
										  .Name = Root["Name"].get<string>(),
										  .DescriptorPath = std::filesystem::absolute(Path).lexically_normal(),
										  .StartupScene = ReadRelativePath(Root, "StartupScene"),
										  .GameModule = ReadRelativePath(Root, "GameModule")};
	if (Root.contains("ContentMounts"))
	{
		Descriptor.ContentMounts.clear();
		for (const Json &Mount : Root.at("ContentMounts"))
		{
			Descriptor.ContentMounts.push_back({.VirtualRoot = Mount.at("VirtualRoot").get<string>(),
												.PhysicalRoot = ReadRelativePath(Mount, "PhysicalRoot"),
												.ReadOnly = Mount.value("ReadOnly", false)});
		}
	}
	if (Root.contains("BuildConfigurations"))
	{
		Descriptor.BuildConfigurations.clear();
		for (const Json &Configuration : Root.at("BuildConfigurations"))
		{
			Descriptor.BuildConfigurations.push_back({.Name = Configuration.at("Name").get<string>(),
													  .Optimized = Configuration.value("Optimized", false),
													  .IncludeDebugSymbols = Configuration.value("IncludeDebugSymbols", true)});
		}
	}
	if (Root.contains("Cook"))
	{
		const Json &Cook = Root.at("Cook");
		Descriptor.Cook = {.CompressionLevel = Cook.value("CompressionLevel", int32{9}),
						   .ArchiveChunkSizeBytes = Cook.value("ArchiveChunkSizeBytes", 64ULL * 1'024ULL * 1'024ULL),
						   .Deterministic = Cook.value("Deterministic", true)};
	}
	if (Root.contains("ArchiveChunks"))
	{
		Descriptor.ArchiveChunks.clear();
		for (const Json &Chunk : Root.at("ArchiveChunks"))
		{
			Descriptor.ArchiveChunks.push_back(
				{.Name = Chunk.at("Name").get<string>(), .VirtualRoots = Chunk.at("VirtualRoots").get<std::vector<string>>()});
		}
	}
	Descriptor.EnabledFeatures = Root.value("EnabledFeatures", std::vector<string>{});
	if (Descriptor.ContentMounts.empty() || Descriptor.BuildConfigurations.empty() || Descriptor.ArchiveChunks.empty())
		throw ProjectDescriptorSerializationException("Project descriptor requires mounts, build configurations, and archive chunks");
	return Descriptor;
}

void ProjectDescriptorSerializer::Save(const project::ProjectDescriptor &Descriptor, const std::filesystem::path &Path)
{
	const std::filesystem::path Destination = Path.empty() ? Descriptor.DescriptorPath : Path;
	if (Destination.empty())
		throw ProjectDescriptorSerializationException("Project descriptor save requires an explicit destination");
	if (!Descriptor.ID.IsValid() || Descriptor.Name.empty())
		throw ProjectDescriptorSerializationException("Project descriptor requires a valid ID and non-empty name");
	if (Descriptor.StartupScene.is_absolute() || Descriptor.GameModule.is_absolute())
		throw ProjectDescriptorSerializationException("Project descriptor scene and module paths must be project-relative");

	Json ContentMounts = Json::array();
	for (const runtime::project::ProjectContentMount &Mount : Descriptor.ContentMounts)
	{
		if (Mount.VirtualRoot.empty() || Mount.VirtualRoot.front() != '/' || Mount.PhysicalRoot.empty() || Mount.PhysicalRoot.is_absolute())
			throw ProjectDescriptorSerializationException("Project content mount is invalid");
		ContentMounts.push_back(
			{{"VirtualRoot", Mount.VirtualRoot}, {"PhysicalRoot", Mount.PhysicalRoot.generic_string()}, {"ReadOnly", Mount.ReadOnly}});
	}
	Json BuildConfigurations = Json::array();
	for (const runtime::project::ProjectBuildConfiguration &Configuration : Descriptor.BuildConfigurations)
	{
		if (Configuration.Name.empty())
			throw ProjectDescriptorSerializationException("Project build configuration name cannot be empty");
		BuildConfigurations.push_back({{"Name", Configuration.Name},
									   {"Optimized", Configuration.Optimized},
									   {"IncludeDebugSymbols", Configuration.IncludeDebugSymbols}});
	}
	Json ArchiveChunks = Json::array();
	for (const runtime::project::ProjectArchiveChunk &Chunk : Descriptor.ArchiveChunks)
	{
		if (Chunk.Name.empty() || Chunk.VirtualRoots.empty())
			throw ProjectDescriptorSerializationException("Project archive chunk requires a name and virtual roots");
		ArchiveChunks.push_back({{"Name", Chunk.Name}, {"VirtualRoots", Chunk.VirtualRoots}});
	}
	const Json Root{{"FormatVersion", CurrentFormatVersion},
					{"ID", Descriptor.ID.ToString()},
					{"EngineSchemaVersion", Descriptor.EngineSchemaVersion},
					{"Name", Descriptor.Name},
					{"ContentMounts", std::move(ContentMounts)},
					{"StartupScene", Descriptor.StartupScene.generic_string()},
					{"GameModule", Descriptor.GameModule.generic_string()},
					{"BuildConfigurations", std::move(BuildConfigurations)},
					{"Cook",
					 {{"CompressionLevel", Descriptor.Cook.CompressionLevel},
					  {"ArchiveChunkSizeBytes", Descriptor.Cook.ArchiveChunkSizeBytes},
					  {"Deterministic", Descriptor.Cook.Deterministic}}},
					{"ArchiveChunks", std::move(ArchiveChunks)},
					{"EnabledFeatures", Descriptor.EnabledFeatures}};
	const std::filesystem::path DirectoryRoot = std::filesystem::absolute(Destination.parent_path()).lexically_normal();
	const std::filesystem::path DestinationRelative = Destination.filename();
	const std::filesystem::path TemporaryRelative = DestinationRelative.string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	const std::filesystem::path Temporary = DirectoryRoot / TemporaryRelative;
	const string Serialized = Root.dump(2) + '\n';
	core::io::SecurePath::CreateTrustedRoot(DirectoryRoot, "project descriptor root");
	core::io::SecurePath::WriteFileWithin(DirectoryRoot, TemporaryRelative, BytesOf(Serialized), false, true,
										  "project descriptor temporary file");
	try
	{
		(void)Load(Temporary);
		core::io::SecurePath::ReplaceWithin(DirectoryRoot, TemporaryRelative, DestinationRelative, "project descriptor publication");
	}
	catch (const std::exception &Exception)
	{
		try
		{
			core::io::SecurePath::RemoveWithin(DirectoryRoot, TemporaryRelative, false, "project descriptor temporary cleanup");
		}
		catch (...)
		{
		}
		throw ProjectDescriptorSerializationException("Could not securely publish project descriptor '" + Destination.string() +
													  "': " + Exception.what());
	}
}
} // namespace editor::serialization
