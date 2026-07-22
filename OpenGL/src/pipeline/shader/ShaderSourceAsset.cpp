#include "ShaderSourceAsset.h"

namespace pipeline::shader
{
ShaderSourceAsset::ShaderSourceAsset(ShaderStage Stage, std::filesystem::path SourcePath, std::string Source,
									 std::vector<std::filesystem::path> Includes)
	: Asset(util::UUID::GenerateRandomUUID()), Stage(Stage), SourcePath(std::move(SourcePath)), Source(std::move(Source)),
	  Includes(std::move(Includes)), SourceHash(static_cast<uint64>(std::hash<std::string>{}(this->Source)))
{
}
ShaderStage ShaderSourceAsset::GetStage() const noexcept
{
	return this->Stage;
}
const std::filesystem::path &ShaderSourceAsset::GetSourcePath() const noexcept
{
	return this->SourcePath;
}
const std::string &ShaderSourceAsset::GetSource() const noexcept
{
	return this->Source;
}
const std::vector<std::filesystem::path> &ShaderSourceAsset::GetIncludes() const noexcept
{
	return this->Includes;
}
uint64 ShaderSourceAsset::GetSourceHash() const noexcept
{
	return this->SourceHash;
}
} // namespace pipeline::shader
