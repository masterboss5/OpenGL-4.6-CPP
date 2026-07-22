#pragma once

#include "ShaderTypes.h"
#include "src/resource/Asset.h"

#include <filesystem>
#include <string>
#include <vector>

namespace pipeline::shader
{
class ShaderSourceAsset final : public resource::Asset
{
  public:
	ShaderSourceAsset(ShaderStage Stage, std::filesystem::path SourcePath, std::string Source, std::vector<std::filesystem::path> Includes);
	[[nodiscard]] ShaderStage GetStage() const noexcept;
	[[nodiscard]] const std::filesystem::path &GetSourcePath() const noexcept;
	[[nodiscard]] const std::string &GetSource() const noexcept;
	[[nodiscard]] const std::vector<std::filesystem::path> &GetIncludes() const noexcept;
	[[nodiscard]] uint64 GetSourceHash() const noexcept;

  private:
	ShaderStage Stage;
	std::filesystem::path SourcePath;
	std::string Source;
	std::vector<std::filesystem::path> Includes;
	uint64 SourceHash;
};
} // namespace pipeline::shader
