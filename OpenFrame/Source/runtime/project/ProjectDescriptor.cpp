#include "ProjectDescriptor.h"

#include "Source/core/io/SecurePath.h"

#include <nlohmann/json.hpp>

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
} // namespace

ProjectDescriptor ProjectDescriptorLoader::Load(const std::filesystem::path &Path)
{
	const std::filesystem::path CanonicalPath = std::filesystem::absolute(Path).lexically_normal();
	try
	{
		constexpr uint64 MaximumProjectDescriptorBytes = 4U * 1024U * 1024U;
		const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileWithin(CanonicalPath.parent_path(), CanonicalPath.filename(),
																			  MaximumProjectDescriptorBytes, "Runtime project descriptor");
		const nlohmann::json Root = nlohmann::json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
		if (!Root.is_object())
			throw ProjectDescriptorException("Project descriptor root must be an object");
		const uint32 FormatVersion = Root.value("FormatVersion", ProjectDescriptorLoader::CurrentFormatVersion);
		if (FormatVersion != ProjectDescriptorLoader::CurrentFormatVersion)
			throw ProjectDescriptorException("Project descriptor format version is unsupported");
		if (!Root.contains("ID") || !Root.at("ID").is_string())
			throw ProjectDescriptorException("Project descriptor requires a persistent ID");

		ProjectDescriptor Result{.FormatVersion = FormatVersion,
								 .ID = util::UUID::Parse(Root.at("ID").get<string>()),
								 .EngineSchemaVersion = Root.value("EngineSchemaVersion", uint32{1}),
								 .Name = Root.value("Name", CanonicalPath.stem().string()),
								 .DescriptorPath = CanonicalPath,
								 .StartupScene = Root.value("StartupScene", string{}),
								 .GameModule = Root.value("GameModule", string{})};

		if (Root.contains("ContentMounts"))
		{
			Result.ContentMounts.clear();
			for (const nlohmann::json &Mount : Root.at("ContentMounts"))
			{
				Result.ContentMounts.push_back({.VirtualRoot = Mount.at("VirtualRoot").get<string>(),
												.PhysicalRoot = Mount.at("PhysicalRoot").get<string>(),
												.ReadOnly = Mount.value("ReadOnly", false)});
			}
		}
		if (Root.contains("BuildConfigurations"))
		{
			Result.BuildConfigurations.clear();
			for (const nlohmann::json &Configuration : Root.at("BuildConfigurations"))
			{
				Result.BuildConfigurations.push_back({.Name = Configuration.at("Name").get<string>(),
													  .Optimized = Configuration.value("Optimized", false),
													  .IncludeDebugSymbols = Configuration.value("IncludeDebugSymbols", true)});
			}
		}
		if (Root.contains("Cook"))
		{
			const nlohmann::json &Cook = Root.at("Cook");
			Result.Cook = {.CompressionLevel = Cook.value("CompressionLevel", int32{9}),
						   .ArchiveChunkSizeBytes = Cook.value("ArchiveChunkSizeBytes", 64ULL * 1'024ULL * 1'024ULL),
						   .Deterministic = Cook.value("Deterministic", true)};
		}
		if (Root.contains("ArchiveChunks"))
		{
			Result.ArchiveChunks.clear();
			for (const nlohmann::json &Chunk : Root.at("ArchiveChunks"))
			{
				Result.ArchiveChunks.push_back(
					{.Name = Chunk.at("Name").get<string>(), .VirtualRoots = Chunk.at("VirtualRoots").get<std::vector<string>>()});
			}
		}
		Result.EnabledFeatures = Root.value("EnabledFeatures", std::vector<string>{});

		if (Result.Name.empty())
			throw ProjectDescriptorException("Project descriptor requires a non-empty Name");
		if (Result.StartupScene.empty())
			throw ProjectDescriptorException("Project descriptor requires a StartupScene for Game");
		if (Result.StartupScene.is_absolute() || (!Result.GameModule.empty() && Result.GameModule.is_absolute()))
			throw ProjectDescriptorException("Project descriptor paths must be relative to the project root");
		if (Result.ContentMounts.empty() || Result.BuildConfigurations.empty() || Result.ArchiveChunks.empty())
			throw ProjectDescriptorException("Project descriptor requires content mounts, build configurations, and archive chunks");
		if (Result.Cook.CompressionLevel < -5 || Result.Cook.CompressionLevel > 22 || Result.Cook.ArchiveChunkSizeBytes == 0)
			throw ProjectDescriptorException("Project cook settings contain an invalid compression level or archive chunk size");
		std::unordered_set<string> MountRoots;
		for (const ProjectContentMount &Mount : Result.ContentMounts)
		{
			if (Mount.VirtualRoot.empty() || Mount.VirtualRoot.front() != '/' || Mount.PhysicalRoot.empty() ||
				Mount.PhysicalRoot.is_absolute())
			{
				throw ProjectDescriptorException("Project content mounts require absolute virtual roots and relative physical roots");
			}
			if (!MountRoots.emplace(Lowercase(Mount.VirtualRoot)).second)
				throw ProjectDescriptorException("Project descriptor contains a duplicate virtual mount root");
		}
		std::unordered_set<string> ConfigurationNames;
		for (const ProjectBuildConfiguration &Configuration : Result.BuildConfigurations)
		{
			if (Configuration.Name.empty() || !ConfigurationNames.emplace(Lowercase(Configuration.Name)).second)
				throw ProjectDescriptorException("Project build configurations require unique non-empty names");
		}
		std::unordered_set<string> ChunkNames;
		for (const ProjectArchiveChunk &Chunk : Result.ArchiveChunks)
		{
			if (Chunk.Name.empty() || Chunk.VirtualRoots.empty() || !ChunkNames.emplace(Lowercase(Chunk.Name)).second)
				throw ProjectDescriptorException("Project archive chunks require unique names and at least one virtual root");
			for (const string &VirtualRoot : Chunk.VirtualRoots)
			{
				const bool KnownMount = std::ranges::any_of(Result.ContentMounts, [&VirtualRoot](const ProjectContentMount &Mount)
															{ return Lowercase(Mount.VirtualRoot) == Lowercase(VirtualRoot); });
				if (!KnownMount)
					throw ProjectDescriptorException("Project archive chunk references an unknown virtual mount root");
			}
		}
		return Result;
	}
	catch (const ProjectDescriptorException &)
	{
		throw;
	}
	catch (const nlohmann::json::exception &Exception)
	{
		throw ProjectDescriptorException("Could not parse project descriptor '" + CanonicalPath.string() + "': " + Exception.what());
	}
	catch (const std::invalid_argument &Exception)
	{
		throw ProjectDescriptorException("Project descriptor '" + CanonicalPath.string() + "' is invalid: " + Exception.what());
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw ProjectDescriptorException("Could not securely read project descriptor '" + CanonicalPath.string() +
										 "': " + Exception.what());
	}
}
} // namespace runtime::project
