#include "ShaderTypes.h"

namespace pipeline::shader
{
	void ShaderPermutationKey::set(ShaderFeature feature, bool enabled) noexcept
	{
		const uint64 bit = uint64 { 1 } << static_cast<uint8>(feature);
		this->featureMask = enabled ? this->featureMask | bit : this->featureMask & ~bit;
	}
	bool ShaderPermutationKey::has(ShaderFeature feature) const noexcept { return (this->featureMask & (uint64 { 1 } << static_cast<uint8>(feature))) != 0; }
	uint64 ShaderPermutationKey::getMask() const noexcept { return this->featureMask; }
	std::string ShaderPermutationKey::getDefineBlock() const
	{
		std::string defines;
		if (has(ShaderFeature::Skinning)) defines += "#define ENGINE_SKINNING 1\n";
		if (has(ShaderFeature::AlphaMask)) defines += "#define ENGINE_ALPHA_MASK 1\n";
		if (has(ShaderFeature::NormalMap)) defines += "#define ENGINE_NORMAL_MAP 1\n";
		return defines;
	}
	std::string ShaderPermutationKey::toString() const { return std::to_string(this->featureMask); }
}
