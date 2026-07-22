#include "MaterialAsset.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace resource
{
namespace
{
void ValidateFactors(const PBRMaterialFactors &Factors)
{
	const auto Finite = [](float32 Value) { return std::isfinite(Value); };
	if (!Finite(Factors.BaseColor.x) || !Finite(Factors.BaseColor.y) || !Finite(Factors.BaseColor.z) || !Finite(Factors.BaseColor.w) ||
		!Finite(Factors.Emissive.x) || !Finite(Factors.Emissive.y) || !Finite(Factors.Emissive.z) || !Finite(Factors.Metallic) ||
		!Finite(Factors.Roughness) || !Finite(Factors.Specular) || !Finite(Factors.NormalScale) || !Finite(Factors.OcclusionStrength) ||
		!Finite(Factors.AlphaCutoff) || !Finite(Factors.IndexOfRefraction))
	{
		throw std::invalid_argument("Material factors must be finite");
	}
	if (Factors.Metallic < 0.0f || Factors.Metallic > 1.0f || Factors.Roughness < 0.0f || Factors.Roughness > 1.0f ||
		Factors.Specular < 0.0f || Factors.Specular > 1.0f || Factors.AlphaCutoff < 0.0f || Factors.AlphaCutoff > 1.0f ||
		Factors.IndexOfRefraction < 1.0f)
	{
		throw std::invalid_argument("Material factors are outside their physical range");
	}
}

void ValidateTextures(const std::vector<MaterialTextureBinding> &Textures)
{
	for (usize Index = 0; Index < Textures.size(); ++Index)
	{
		if (!Textures[Index].Texture)
			throw std::invalid_argument("Material texture binding requires a valid asset handle");
		if (Textures[Index].TextureCoordinateChannel >= MaterialTextureCoordinateChannelCount)
			throw std::invalid_argument("Material texture binding references an unsupported texture-coordinate channel");
		for (usize Prior = 0; Prior < Index; ++Prior)
		{
			if (Textures[Prior].Semantic == Textures[Index].Semantic)
			{
				throw std::invalid_argument("Material texture semantics must be unique");
			}
		}
	}
}
} // namespace

MaterialInterfaceAsset::MaterialInterfaceAsset(string Name, MaterialPipelineContract PipelineContract)
	: Asset(util::UUID::GenerateRandomUUID()), Name(std::move(Name)), PipelineContract(PipelineContract)
{
	if (this->Name.empty())
		throw std::invalid_argument("Material requires a non-empty name");
}

MaterialAsset::MaterialAsset(string Name, MaterialPipelineContract PipelineContract, PBRMaterialFactors Factors,
							 std::vector<MaterialTextureBinding> Textures)
	: MaterialInterfaceAsset(std::move(Name), PipelineContract), Factors(Factors), Textures(std::move(Textures))
{
	ValidateFactors(this->Factors);
	ValidateTextures(this->Textures);
}

MaterialInstanceAsset::MaterialInstanceAsset(string Name, AssetHandle<MaterialInterfaceAsset> Parent,
											 MaterialPipelineContract PipelineContract, PBRMaterialFactors ResolvedFactors,
											 std::vector<MaterialTextureBinding> ResolvedTextures)
	: MaterialInterfaceAsset(std::move(Name), PipelineContract), Parent(std::move(Parent)), ResolvedFactors(ResolvedFactors),
	  ResolvedTextures(std::move(ResolvedTextures))
{
	if (!this->Parent)
		throw std::invalid_argument("Material instance requires a parent material handle");
	ValidateFactors(this->ResolvedFactors);
	ValidateTextures(this->ResolvedTextures);
}
} // namespace resource
