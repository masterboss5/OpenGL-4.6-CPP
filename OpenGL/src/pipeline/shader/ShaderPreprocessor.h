#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ShaderSourceAsset.h"

namespace pipeline::shader
{
	struct ShaderPreprocessResult final { std::string source; std::vector<std::filesystem::path> dependencies; };
	class ShaderPreprocessor final
	{
	public:
		[[nodiscard]] ShaderPreprocessResult preprocess(const ShaderSourceAsset& source, const ShaderPermutationKey& permutation) const;
	};
}
