#pragma once

#include "src/types.h"

#include <filesystem>
#include <string>

namespace pipeline::shader
{
enum class ShaderStage : uint8
{
	Vertex,
	Fragment,
	Compute
};
enum class ShaderFeature : uint8
{
	Skinning,
	AlphaMask,
	NormalMap,
	ManualSRGBEncode
};

class ShaderPermutationKey final
{
  public:
	void Set(ShaderFeature Feature, bool Enabled) noexcept;
	[[nodiscard]] bool Has(ShaderFeature Feature) const noexcept;
	[[nodiscard]] uint64 GetMask() const noexcept;
	[[nodiscard]] std::string GetDefineBlock() const;
	[[nodiscard]] std::string ToString() const;
	[[nodiscard]] bool operator==(const ShaderPermutationKey &) const noexcept = default;

  private:
	uint64 FeatureMask = 0;
};

struct ShaderSourceDescriptor final
{
	std::filesystem::path Path;
	ShaderStage Stage;
};
} // namespace pipeline::shader
