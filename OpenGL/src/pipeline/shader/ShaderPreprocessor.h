#pragma once

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
class ShaderPreprocessor final
{
  public:
	[[nodiscard]] ShaderPreprocessResult Preprocess(const ShaderSourceAsset &Source, const ShaderPermutationKey &Permutation) const;
};
} // namespace pipeline::shader
