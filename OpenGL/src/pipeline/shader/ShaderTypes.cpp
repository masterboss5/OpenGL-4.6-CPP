#include "ShaderTypes.h"

namespace pipeline::shader
{
void ShaderPermutationKey::Set(ShaderFeature Feature, bool Enabled) noexcept
{
	const uint64 Bit = uint64{1} << static_cast<uint8>(Feature);
	this->FeatureMask = Enabled ? this->FeatureMask | Bit : this->FeatureMask & ~Bit;
}
bool ShaderPermutationKey::Has(ShaderFeature Feature) const noexcept
{
	return (this->FeatureMask & (uint64{1} << static_cast<uint8>(Feature))) != 0;
}
uint64 ShaderPermutationKey::GetMask() const noexcept
{
	return this->FeatureMask;
}
std::string ShaderPermutationKey::GetDefineBlock() const
{
	std::string Defines;
	if (Has(ShaderFeature::Skinning))
		Defines += "#define ENGINE_SKINNING 1\n";
	if (Has(ShaderFeature::AlphaMask))
		Defines += "#define ENGINE_ALPHA_MASK 1\n";
	if (Has(ShaderFeature::NormalMap))
		Defines += "#define ENGINE_NORMAL_MAP 1\n";
	if (Has(ShaderFeature::ManualSRGBEncode))
		Defines += "#define ENGINE_MANUAL_SRGB_ENCODE 1\n";
	return Defines;
}
std::string ShaderPermutationKey::ToString() const
{
	return std::to_string(this->FeatureMask);
}
} // namespace pipeline::shader
