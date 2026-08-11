#pragma once

#include "src/core/EngineAPI.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <filesystem>
#include <stdexcept>
#include <vector>

namespace runtime::project
{
struct ProjectContentMount final
{
	string VirtualRoot;
	std::filesystem::path PhysicalRoot;
	bool ReadOnly = false;
};

struct ProjectBuildConfiguration final
{
	string Name;
	bool Optimized = false;
	bool IncludeDebugSymbols = true;
};

struct ProjectCookSettings final
{
	int32 CompressionLevel = 9;
	uint64 ArchiveChunkSizeBytes = 64ULL * 1'024ULL * 1'024ULL;
	bool Deterministic = true;
};

struct ProjectArchiveChunk final
{
	string Name;
	std::vector<string> VirtualRoots;
};

struct ProjectDescriptor final
{
	uint32 FormatVersion = 1;
	util::UUID ID = util::UUID::GenerateRandomUUID();
	uint32 EngineSchemaVersion = 1;
	string Name;
	std::filesystem::path DescriptorPath;
	std::vector<ProjectContentMount> ContentMounts{{.VirtualRoot = "/Game", .PhysicalRoot = "Content", .ReadOnly = false},
												   {.VirtualRoot = "/Engine", .PhysicalRoot = "Engine", .ReadOnly = true}};
	std::filesystem::path StartupScene;
	std::filesystem::path GameModule;
	std::vector<ProjectBuildConfiguration> BuildConfigurations{{.Name = "Development", .Optimized = false, .IncludeDebugSymbols = true},
															   {.Name = "Shipping", .Optimized = true, .IncludeDebugSymbols = false}};
	ProjectCookSettings Cook;
	std::vector<ProjectArchiveChunk> ArchiveChunks{{.Name = "Main", .VirtualRoots = {"/Game"}}};
	std::vector<string> EnabledFeatures;
};

class ENGINE_API ProjectDescriptorException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API ProjectDescriptorLoader final
{
  public:
	static constexpr uint32 CurrentFormatVersion = 1;

	[[nodiscard]] static ProjectDescriptor Load(const std::filesystem::path &Path);
};
} // namespace runtime::project
