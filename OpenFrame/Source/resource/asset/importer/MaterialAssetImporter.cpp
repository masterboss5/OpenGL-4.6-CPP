#include "MaterialAssetImporter.h"

#include "Source/resource/asset/MaterialAsset.h"

#include <nlohmann/json.hpp>

#include <array>
#include <unordered_map>

namespace resource::importer
{
namespace
{
using Json = nlohmann::json;

constexpr uint32 MaterialFormatVersion = 2;

template <typename EnumType, usize Size>
[[nodiscard]] EnumType ParseEnum(const Json &Root, const string_view Field,
								 const std::array<std::pair<string_view, EnumType>, Size> &Options)
{
	const string Value = Root.at(Field).get<string>();
	const auto Found = std::ranges::find_if(Options, [&Value](const auto &Option) { return Option.first == Value; });
	if (Found == Options.end())
		throw std::invalid_argument("Unknown " + string(Field) + " value '" + Value + "'");
	return Found->second;
}

[[nodiscard]] glm::vec3 ParseVector3(const Json &Root, const string_view Field, const glm::vec3 Default)
{
	const auto Found = Root.find(Field);
	if (Found == Root.end())
		return Default;
	if (!Found->is_array() || Found->size() != 3)
		throw std::invalid_argument(string(Field) + " must contain exactly three numbers");
	return {Found->at(0).get<float32>(), Found->at(1).get<float32>(), Found->at(2).get<float32>()};
}

[[nodiscard]] glm::vec4 ParseVector4(const Json &Root, const string_view Field, const glm::vec4 Default)
{
	const auto Found = Root.find(Field);
	if (Found == Root.end())
		return Default;
	if (!Found->is_array() || Found->size() != 4)
		throw std::invalid_argument(string(Field) + " must contain exactly four numbers");
	return {Found->at(0).get<float32>(), Found->at(1).get<float32>(), Found->at(2).get<float32>(), Found->at(3).get<float32>()};
}

[[nodiscard]] MaterialPipelineContract ParsePipeline(const Json &Root, const MaterialPipelineContract Defaults = {})
{
	static constexpr std::array<std::pair<string_view, MaterialShadingModel>, 6> ShadingModels{{
		{"Unlit", MaterialShadingModel::Unlit},
		{"DefaultLit", MaterialShadingModel::DefaultLit},
		{"Subsurface", MaterialShadingModel::Subsurface},
		{"ClearCoat", MaterialShadingModel::ClearCoat},
		{"Cloth", MaterialShadingModel::Cloth},
		{"Hair", MaterialShadingModel::Hair},
	}};
	static constexpr std::array<std::pair<string_view, MaterialBlendMode>, 4> BlendModes{{
		{"Opaque", MaterialBlendMode::Opaque},
		{"Masked", MaterialBlendMode::Masked},
		{"Translucent", MaterialBlendMode::Translucent},
		{"Additive", MaterialBlendMode::Additive},
	}};
	const Json Pipeline = Root.value("Pipeline", Json::object());
	return {.ShadingModel = Pipeline.contains("ShadingModel") ? ParseEnum(Pipeline, "ShadingModel", ShadingModels) : Defaults.ShadingModel,
			.BlendMode = Pipeline.contains("BlendMode") ? ParseEnum(Pipeline, "BlendMode", BlendModes) : Defaults.BlendMode,
			.TwoSided = Pipeline.value("TwoSided", Defaults.TwoSided),
			.CastsShadows = Pipeline.value("CastsShadows", Defaults.CastsShadows),
			.ReceivesShadows = Pipeline.value("ReceivesShadows", Defaults.ReceivesShadows)};
}

[[nodiscard]] PBRMaterialFactors ParseFactors(const Json &Root, PBRMaterialFactors Result = {})
{
	const Json Factors = Root.value("Factors", Json::object());
	Result.BaseColor = ParseVector4(Factors, "BaseColor", Result.BaseColor);
	Result.Emissive = ParseVector3(Factors, "Emissive", Result.Emissive);
	Result.Metallic = Factors.value("Metallic", Result.Metallic);
	Result.Roughness = Factors.value("Roughness", Result.Roughness);
	Result.Specular = Factors.value("Specular", Result.Specular);
	Result.NormalScale = Factors.value("NormalScale", Result.NormalScale);
	Result.OcclusionStrength = Factors.value("OcclusionStrength", Result.OcclusionStrength);
	Result.AlphaCutoff = Factors.value("AlphaCutoff", Result.AlphaCutoff);
	Result.ClearCoat = Factors.value("ClearCoat", Result.ClearCoat);
	Result.ClearCoatRoughness = Factors.value("ClearCoatRoughness", Result.ClearCoatRoughness);
	Result.Transmission = Factors.value("Transmission", Result.Transmission);
	Result.IndexOfRefraction = Factors.value("IndexOfRefraction", Result.IndexOfRefraction);
	return Result;
}

[[nodiscard]] MaterialTextureSemantic ParseTextureSemantic(const Json &Root)
{
	static constexpr std::array<std::pair<string_view, MaterialTextureSemantic>, 9> Semantics{{
		{"BaseColor", MaterialTextureSemantic::BaseColor},
		{"Normal", MaterialTextureSemantic::Normal},
		{"MetallicRoughness", MaterialTextureSemantic::MetallicRoughness},
		{"Occlusion", MaterialTextureSemantic::Occlusion},
		{"Emissive", MaterialTextureSemantic::Emissive},
		{"Specular", MaterialTextureSemantic::Specular},
		{"ClearCoat", MaterialTextureSemantic::ClearCoat},
		{"ClearCoatNormal", MaterialTextureSemantic::ClearCoatNormal},
		{"Transmission", MaterialTextureSemantic::Transmission},
	}};
	return ParseEnum(Root, "Semantic", Semantics);
}

struct ParsedTextures final
{
	std::vector<MaterialTextureBinding> Bindings;
	std::vector<AssetDependency> Dependencies;
};

[[nodiscard]] ParsedTextures ParseTextures(const Json &Root, AssetImportContext &Context)
{
	ParsedTextures Result;
	const Json Textures = Root.value("Textures", Json::array());
	if (!Textures.is_array())
		throw std::invalid_argument("Textures must be an array");
	Result.Bindings.reserve(Textures.size());
	Result.Dependencies.reserve(Textures.size());
	for (const Json &Texture : Textures)
	{
		if (!Texture.is_object())
			throw std::invalid_argument("Texture binding must be an object");
		const AssetID ID = Texture.at("AssetID").get<string>();
		if (ID.empty())
			throw std::invalid_argument("Texture binding AssetID cannot be empty");
		ImportedAssetReference<Texture2DAsset> Reference = Context.Resolve<Texture2DAsset>(AssetType::Texture2D, ID);
		Result.Dependencies.push_back({AssetType::Texture2D, Reference.CanonicalPath});
		Result.Bindings.push_back({.Semantic = ParseTextureSemantic(Texture),
								   .Texture = std::move(Reference.Handle),
								   .TextureCoordinateChannel = Texture.value("TextureCoordinateChannel", 0U)});
	}
	return Result;
}

[[nodiscard]] Json ParseRoot(const string &Source, const std::filesystem::path &Path, const AssetType ExpectedType,
							 const string_view ExpectedName)
{
	try
	{
		const Json Root = Json::parse(Source, nullptr, true, true);
		if (!Root.is_object())
			throw std::invalid_argument("Material root must be an object");
		const uint32 Version = Root.value("FormatVersion", 0U);
		if (Version != 1U && Version != MaterialFormatVersion)
			throw std::invalid_argument("Material format version is unsupported");
		if (Root.value("AssetType", string{}) != ExpectedName)
			throw std::invalid_argument("Material AssetType does not match its file extension");
		if (Root.value("Name", string{}).empty())
			throw std::invalid_argument("Material Name cannot be empty");
		return Root;
	}
	catch (const AssetImportException &)
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		throw AssetMaterialParseException(ExpectedType, Path, Exception.what());
	}
}
} // namespace

bool MaterialAssetImporter::CanImport(const std::filesystem::path &Path) const
{
	return GetNormalizedExtension(Path) == ".material";
}

AssetType MaterialAssetImporter::GetAssetType() const noexcept
{
	return AssetType::Material;
}

AssetImportResult MaterialAssetImporter::ImportCPU(const std::filesystem::path &Path, AssetImportContext &Context) const
{
	try
	{
		this->ValidateImportRequest(Path);
		const Json Root = ParseRoot(this->ReadTextSource(Path), Path, this->GetAssetType(), "Material");
		ParsedTextures Textures = ParseTextures(Root, Context);
		return {AssetPtr<MaterialAsset>::Make(Root.at("Name").get<string>(), ParsePipeline(Root), ParseFactors(Root),
											  std::move(Textures.Bindings)),
				std::move(Textures.Dependencies)};
	}
	catch (const AssetImportException &)
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		throw AssetMaterialParseException(this->GetAssetType(), Path, Exception.what());
	}
}

bool MaterialInstanceAssetImporter::CanImport(const std::filesystem::path &Path) const
{
	return GetNormalizedExtension(Path) == ".materialinstance";
}

AssetType MaterialInstanceAssetImporter::GetAssetType() const noexcept
{
	return AssetType::MaterialInstance;
}

AssetImportResult MaterialInstanceAssetImporter::ImportCPU(const std::filesystem::path &Path, AssetImportContext &Context) const
{
	try
	{
		this->ValidateImportRequest(Path);
		const Json Root = ParseRoot(this->ReadTextSource(Path), Path, this->GetAssetType(), "MaterialInstance");
		const Json Parent = Root.at("Parent");
		if (!Parent.is_object())
			throw std::invalid_argument("Material instance Parent must be an object");
		const AssetID ParentID = Parent.at("AssetID").get<string>();
		const string ParentTypeName = Parent.value("AssetType", string("Material"));
		const AssetType ParentType = ParentTypeName == "Material"
										 ? AssetType::Material
										 : (ParentTypeName == "MaterialInstance" ? AssetType::MaterialInstance : AssetType::Count);
		if (ParentType == AssetType::Count)
			throw std::invalid_argument("Material instance parent type must be Material or MaterialInstance");
		ImportedAssetReference<MaterialInterfaceAsset> ParentReference = Context.Resolve<MaterialInterfaceAsset>(ParentType, ParentID);
		const AssetPtr<MaterialInterfaceAsset> ParentAsset = ParentReference.Handle.Pin();
		if (ParentAsset == nullptr)
			throw std::invalid_argument("Material instance parent asset is unavailable");
		ParsedTextures Textures = ParseTextures(Root, Context);
		std::vector<MaterialTextureBinding> ResolvedTextures(ParentAsset->GetTextures().begin(), ParentAsset->GetTextures().end());
		for (MaterialTextureBinding &Override : Textures.Bindings)
		{
			const auto Existing = std::ranges::find(ResolvedTextures, Override.Semantic, &MaterialTextureBinding::Semantic);
			if (Existing == ResolvedTextures.end())
				ResolvedTextures.push_back(std::move(Override));
			else
				*Existing = std::move(Override);
		}
		Textures.Dependencies.push_back({ParentType, ParentReference.CanonicalPath});
		MaterialPipelineContract ResolvedPipeline = ParsePipeline(Root, ParentAsset->GetPipelineContract());
		PBRMaterialFactors ResolvedFactors = ParseFactors(Root, ParentAsset->GetFactors());
		const Json PipelineOverrides = Root.value("Pipeline", Json::object());
		const Json FactorOverrides = Root.value("Factors", Json::object());
		if (PipelineOverrides.contains("ShadingModel") && ResolvedPipeline.ShadingModel != MaterialShadingModel::ClearCoat &&
			!FactorOverrides.contains("ClearCoat"))
		{
			ResolvedFactors.ClearCoat = 0.0f;
			if (!FactorOverrides.contains("ClearCoatRoughness"))
				ResolvedFactors.ClearCoatRoughness = 0.0f;
		}
		if (PipelineOverrides.contains("BlendMode") && ResolvedPipeline.BlendMode != MaterialBlendMode::Translucent &&
			!FactorOverrides.contains("Transmission"))
		{
			ResolvedFactors.Transmission = 0.0f;
		}
		return {AssetPtr<MaterialInstanceAsset>::Make(Root.at("Name").get<string>(), std::move(ParentReference.Handle), ResolvedPipeline,
													  ResolvedFactors, std::move(ResolvedTextures)),
				std::move(Textures.Dependencies)};
	}
	catch (const AssetImportException &)
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		throw AssetMaterialParseException(this->GetAssetType(), Path, Exception.what());
	}
}
} // namespace resource::importer
