#pragma once

#include "src/resource/Asset.h"
#include "src/resource/asset/AssetHandle.h"
#include "src/resource/asset/AssetTypes.h"
#include "src/resource/asset/Texture2DAsset.h"

#include <glm.hpp>
#include <span>
#include <vector>

namespace resource
{
inline constexpr uint32 MaterialTextureCoordinateChannelCount = 4;

enum class MaterialShadingModel : uint8
{
	Unlit,
	DefaultLit,
	Subsurface,
	ClearCoat,
	Cloth,
	Hair
};

enum class MaterialBlendMode : uint8
{
	Opaque,
	Masked,
	Translucent,
	Additive
};

enum class MaterialTextureSemantic : uint8
{
	BaseColor,
	Normal,
	MetallicRoughness,
	Occlusion,
	Emissive,
	Specular,
	ClearCoat,
	ClearCoatNormal,
	Transmission
};

struct MaterialPipelineContract final
{
	MaterialShadingModel ShadingModel = MaterialShadingModel::DefaultLit;
	MaterialBlendMode BlendMode = MaterialBlendMode::Opaque;
	bool TwoSided = false;
	bool CastsShadows = true;
	bool ReceivesShadows = true;
};

struct PBRMaterialFactors final
{
	glm::vec4 BaseColor{1.0f};
	glm::vec3 Emissive{0.0f};
	float32 Metallic = 0.0f;
	float32 Roughness = 1.0f;
	float32 Specular = 1.0f;
	float32 NormalScale = 1.0f;
	float32 OcclusionStrength = 1.0f;
	float32 AlphaCutoff = 0.5f;
	float32 ClearCoat = 0.0f;
	float32 ClearCoatRoughness = 0.0f;
	float32 Transmission = 0.0f;
	float32 IndexOfRefraction = 1.5f;
};

struct MaterialTextureBinding final
{
	MaterialTextureSemantic Semantic = MaterialTextureSemantic::BaseColor;
	AssetHandle<Texture2DAsset> Texture;
	uint32 TextureCoordinateChannel = 0;
};

class MaterialInterfaceAsset : public Asset
{
  public:
	[[nodiscard]] string_view GetName() const noexcept
	{
		return this->Name;
	}
	[[nodiscard]] const MaterialPipelineContract &GetPipelineContract() const noexcept
	{
		return this->PipelineContract;
	}
	virtual const PBRMaterialFactors &GetFactors() const noexcept = 0;
	virtual std::span<const MaterialTextureBinding> GetTextures() const noexcept = 0;

  protected:
	MaterialInterfaceAsset(string Name, MaterialPipelineContract PipelineContract);
	~MaterialInterfaceAsset() override = default;

  private:
	string Name;
	MaterialPipelineContract PipelineContract;
};

class MaterialAsset final : public MaterialInterfaceAsset
{
  public:
	inline static constexpr resource::AssetType AssetType = resource::AssetType::Material;

	MaterialAsset(string Name, MaterialPipelineContract PipelineContract, PBRMaterialFactors Factors,
				  std::vector<MaterialTextureBinding> Textures);

	[[nodiscard]] const PBRMaterialFactors &GetFactors() const noexcept override
	{
		return this->Factors;
	}
	[[nodiscard]] std::span<const MaterialTextureBinding> GetTextures() const noexcept override
	{
		return this->Textures;
	}

  private:
	PBRMaterialFactors Factors;
	std::vector<MaterialTextureBinding> Textures;
};

struct MaterialFactorOverride final
{
	PBRMaterialFactors Factors;
};

class MaterialInstanceAsset final : public MaterialInterfaceAsset
{
  public:
	inline static constexpr AssetType AssetType = AssetType::MaterialInstance;

	MaterialInstanceAsset(string Name, AssetHandle<MaterialInterfaceAsset> Parent, MaterialPipelineContract PipelineContract,
						  PBRMaterialFactors ResolvedFactors, std::vector<MaterialTextureBinding> ResolvedTextures);

	[[nodiscard]] const AssetHandle<MaterialInterfaceAsset> &GetParent() const noexcept
	{
		return this->Parent;
	}
	[[nodiscard]] const PBRMaterialFactors &GetFactors() const noexcept override
	{
		return this->ResolvedFactors;
	}
	[[nodiscard]] std::span<const MaterialTextureBinding> GetTextures() const noexcept override
	{
		return this->ResolvedTextures;
	}

  private:
	AssetHandle<MaterialInterfaceAsset> Parent;
	PBRMaterialFactors ResolvedFactors;
	std::vector<MaterialTextureBinding> ResolvedTextures;
};
} // namespace resource
