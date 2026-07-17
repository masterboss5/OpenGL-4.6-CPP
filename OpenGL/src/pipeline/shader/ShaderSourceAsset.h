#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ShaderTypes.h"
#include "src/resource/Asset.h"

namespace pipeline::shader
{
	class ShaderSourceAsset final : public resource::Asset
	{
	public:
		ShaderSourceAsset(ShaderStage stage, std::filesystem::path sourcePath, std::string source, std::vector<std::filesystem::path> includes);
		[[nodiscard]] ShaderStage getStage() const noexcept;
		[[nodiscard]] const std::filesystem::path& getSourcePath() const noexcept;
		[[nodiscard]] const std::string& getSource() const noexcept;
		[[nodiscard]] const std::vector<std::filesystem::path>& getIncludes() const noexcept;
		[[nodiscard]] uint64 getSourceHash() const noexcept;
	private:
		ShaderStage stage;
		std::filesystem::path sourcePath;
		std::string source;
		std::vector<std::filesystem::path> includes;
		uint64 sourceHash;
	};
}
