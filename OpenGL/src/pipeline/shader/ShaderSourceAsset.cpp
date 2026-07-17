#include "ShaderSourceAsset.h"

namespace pipeline::shader
{
	ShaderSourceAsset::ShaderSourceAsset(ShaderStage stage, std::filesystem::path sourcePath, std::string source, std::vector<std::filesystem::path> includes)
		: Asset(util::UUID::generateRandomUUID()), stage(stage), sourcePath(std::move(sourcePath)), source(std::move(source)), includes(std::move(includes)), sourceHash(static_cast<uint64>(std::hash<std::string>{}(this->source))) {}
	ShaderStage ShaderSourceAsset::getStage() const noexcept { return this->stage; }
	const std::filesystem::path& ShaderSourceAsset::getSourcePath() const noexcept { return this->sourcePath; }
	const std::string& ShaderSourceAsset::getSource() const noexcept { return this->source; }
	const std::vector<std::filesystem::path>& ShaderSourceAsset::getIncludes() const noexcept { return this->includes; }
	uint64 ShaderSourceAsset::getSourceHash() const noexcept { return this->sourceHash; }
}
