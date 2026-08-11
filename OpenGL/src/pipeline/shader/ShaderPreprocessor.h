#pragma once

#include "src/core/EngineAPI.h"

#include "ShaderSourceAsset.h"

#include <filesystem>
#include <string>
#include <vector>

namespace pipeline::shader
{
struct ShaderPreprocessResult final
{
	std::string Source;
	std::vector<std::filesystem::path> Dependencies;
};
class ENGINE_API ShaderPreprocessor final
{
  public:
	[[nodiscard]] ShaderPreprocessResult Preprocess(const ShaderSourceAsset &Source, const ShaderPermutationKey &Permutation,
													const std::filesystem::path &TrustedRoot) const;
};
} // namespace pipeline::shader
