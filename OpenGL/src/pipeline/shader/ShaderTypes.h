#pragma once

#include <filesystem>
#include <string>

#include "src/types.h"

namespace pipeline::shader
{
	enum class ShaderStage : uint8 { Vertex, Fragment, Compute };
	enum class ShaderFeature : uint8 { Skinning, AlphaMask, NormalMap };

	class ShaderPermutationKey final
	{
	public:
		void set(ShaderFeature feature, bool enabled) noexcept;
		[[nodiscard]] bool has(ShaderFeature feature) const noexcept;
		[[nodiscard]] uint64 getMask() const noexcept;
		[[nodiscard]] std::string getDefineBlock() const;
		[[nodiscard]] std::string toString() const;
		[[nodiscard]] bool operator==(const ShaderPermutationKey&) const noexcept = default;
	private:
		uint64 featureMask = 0;
	};

	struct ShaderSourceDescriptor final { std::filesystem::path path; ShaderStage stage; };
}
