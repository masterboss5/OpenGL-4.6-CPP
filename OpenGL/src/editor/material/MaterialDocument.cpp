#include "MaterialDocument.h"

#include "src/core/io/SecurePath.h"
#include "src/resource/asset/AssetManager.h"
#include "src/util/UUID.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <system_error>
#include <unordered_set>

namespace editor::material
{
namespace
{
using Json = nlohmann::json;

template <typename EnumType, usize Size>
[[nodiscard]] EnumType ParseEnum(const string &Value, const std::array<std::pair<string_view, EnumType>, Size> &Options,
								 const string_view Field)
{
	const auto Found = std::ranges::find_if(Options, [&Value](const auto &Option) { return Option.first == Value; });
	if (Found == Options.end())
		throw MaterialDocumentException("Unknown " + string(Field) + " value '" + Value + "'");
	return Found->second;
}

template <typename EnumType, usize Size>
[[nodiscard]] string_view EnumName(const EnumType Value, const std::array<std::pair<string_view, EnumType>, Size> &Options,
								   const string_view Field)
{
	const auto Found = std::ranges::find_if(Options, [Value](const auto &Option) { return Option.second == Value; });
	if (Found == Options.end())
		throw MaterialDocumentException("Cannot serialize unknown " + string(Field));
	return Found->first;
}

constexpr std::array<std::pair<string_view, resource::MaterialShadingModel>, 6> ShadingModels{{
	{"Unlit", resource::MaterialShadingModel::Unlit},
	{"DefaultLit", resource::MaterialShadingModel::DefaultLit},
	{"Subsurface", resource::MaterialShadingModel::Subsurface},
	{"ClearCoat", resource::MaterialShadingModel::ClearCoat},
	{"Cloth", resource::MaterialShadingModel::Cloth},
	{"Hair", resource::MaterialShadingModel::Hair},
}};
constexpr std::array<std::pair<string_view, resource::MaterialBlendMode>, 4> BlendModes{{
	{"Opaque", resource::MaterialBlendMode::Opaque},
	{"Masked", resource::MaterialBlendMode::Masked},
	{"Translucent", resource::MaterialBlendMode::Translucent},
	{"Additive", resource::MaterialBlendMode::Additive},
}};
constexpr std::array<std::pair<string_view, resource::MaterialTextureSemantic>, 9> TextureSemantics{{
	{"BaseColor", resource::MaterialTextureSemantic::BaseColor},
	{"Normal", resource::MaterialTextureSemantic::Normal},
	{"MetallicRoughness", resource::MaterialTextureSemantic::MetallicRoughness},
	{"Occlusion", resource::MaterialTextureSemantic::Occlusion},
	{"Emissive", resource::MaterialTextureSemantic::Emissive},
	{"Specular", resource::MaterialTextureSemantic::Specular},
	{"ClearCoat", resource::MaterialTextureSemantic::ClearCoat},
	{"ClearCoatNormal", resource::MaterialTextureSemantic::ClearCoatNormal},
	{"Transmission", resource::MaterialTextureSemantic::Transmission},
}};

[[nodiscard]] glm::vec3 ReadVector3(const Json &Value, const string_view Field)
{
	if (!Value.is_array() || Value.size() != 3)
		throw MaterialDocumentException(string(Field) + " must contain exactly three numbers");
	return {Value.at(0).get<float32>(), Value.at(1).get<float32>(), Value.at(2).get<float32>()};
}

[[nodiscard]] glm::vec4 ReadVector4(const Json &Value, const string_view Field)
{
	if (!Value.is_array() || Value.size() != 4)
		throw MaterialDocumentException(string(Field) + " must contain exactly four numbers");
	return {Value.at(0).get<float32>(), Value.at(1).get<float32>(), Value.at(2).get<float32>(), Value.at(3).get<float32>()};
}

} // namespace

bool MaterialFactorOverrides::Empty() const noexcept
{
	return !this->BaseColor && !this->Emissive && !this->Metallic && !this->Roughness && !this->Specular && !this->NormalScale &&
		   !this->OcclusionStrength && !this->AlphaCutoff && !this->ClearCoat && !this->ClearCoatRoughness && !this->Transmission &&
		   !this->IndexOfRefraction;
}

void MaterialFactorOverrides::Apply(resource::PBRMaterialFactors &Factors) const noexcept
{
	if (this->BaseColor)
		Factors.BaseColor = *this->BaseColor;
	if (this->Emissive)
		Factors.Emissive = *this->Emissive;
	if (this->Metallic)
		Factors.Metallic = *this->Metallic;
	if (this->Roughness)
		Factors.Roughness = *this->Roughness;
	if (this->Specular)
		Factors.Specular = *this->Specular;
	if (this->NormalScale)
		Factors.NormalScale = *this->NormalScale;
	if (this->OcclusionStrength)
		Factors.OcclusionStrength = *this->OcclusionStrength;
	if (this->AlphaCutoff)
		Factors.AlphaCutoff = *this->AlphaCutoff;
	if (this->ClearCoat)
		Factors.ClearCoat = *this->ClearCoat;
	if (this->ClearCoatRoughness)
		Factors.ClearCoatRoughness = *this->ClearCoatRoughness;
	if (this->Transmission)
		Factors.Transmission = *this->Transmission;
	if (this->IndexOfRefraction)
		Factors.IndexOfRefraction = *this->IndexOfRefraction;
}

MaterialDocument MaterialDocumentStore::Load(const std::filesystem::path &Path)
{
	try
	{
		constexpr uint64 MaximumMaterialDocumentBytes = 16U * 1024U * 1024U;
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(Path.parent_path(), Path.filename(), MaximumMaterialDocumentBytes, "Material document");
		const Json Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
		if (!Root.is_object())
			throw MaterialDocumentException("Material document root must be an object");
		const string AssetTypeName = Root.at("AssetType").get<string>();
		const uint32 StoredVersion = Root.at("FormatVersion").get<uint32>();
		MaterialDocument Result{.FormatVersion = MaterialDocument::CurrentFormatVersion,
								.DocumentID = Root.value("DocumentID", util::UUID::GenerateRandomUUID().ToString()),
								.Type =
									AssetTypeName == "Material" ? MaterialDocumentType::Material : MaterialDocumentType::MaterialInstance,
								.Name = Root.at("Name").get<string>(),
								.Path = std::filesystem::absolute(Path).lexically_normal()};
		if (AssetTypeName != "Material" && AssetTypeName != "MaterialInstance")
			throw MaterialDocumentException("Material document AssetType is invalid");
		const Json Pipeline = Root.value("Pipeline", Json::object());
		Result.Pipeline = {.ShadingModel = ParseEnum(Pipeline.value("ShadingModel", string("DefaultLit")), ShadingModels, "ShadingModel"),
						   .BlendMode = ParseEnum(Pipeline.value("BlendMode", string("Opaque")), BlendModes, "BlendMode"),
						   .TwoSided = Pipeline.value("TwoSided", false),
						   .CastsShadows = Pipeline.value("CastsShadows", true),
						   .ReceivesShadows = Pipeline.value("ReceivesShadows", true)};
		if (Result.Type == MaterialDocumentType::MaterialInstance && Root.contains("Pipeline"))
			Result.PipelineOverride = Result.Pipeline;
		const Json Factors = Root.value("Factors", Json::object());
		Result.Factors.BaseColor =
			Factors.contains("BaseColor") ? ReadVector4(Factors.at("BaseColor"), "BaseColor") : Result.Factors.BaseColor;
		Result.Factors.Emissive = Factors.contains("Emissive") ? ReadVector3(Factors.at("Emissive"), "Emissive") : Result.Factors.Emissive;
		Result.Factors.Metallic = Factors.value("Metallic", Result.Factors.Metallic);
		Result.Factors.Roughness = Factors.value("Roughness", Result.Factors.Roughness);
		Result.Factors.Specular = Factors.value("Specular", Result.Factors.Specular);
		Result.Factors.NormalScale = Factors.value("NormalScale", Result.Factors.NormalScale);
		Result.Factors.OcclusionStrength = Factors.value("OcclusionStrength", Result.Factors.OcclusionStrength);
		Result.Factors.AlphaCutoff = Factors.value("AlphaCutoff", Result.Factors.AlphaCutoff);
		Result.Factors.ClearCoat = Factors.value("ClearCoat", Result.Factors.ClearCoat);
		Result.Factors.ClearCoatRoughness = Factors.value("ClearCoatRoughness", Result.Factors.ClearCoatRoughness);
		Result.Factors.Transmission = Factors.value("Transmission", Result.Factors.Transmission);
		Result.Factors.IndexOfRefraction = Factors.value("IndexOfRefraction", Result.Factors.IndexOfRefraction);
		if (Result.Type == MaterialDocumentType::MaterialInstance)
		{
			if (Factors.contains("BaseColor"))
				Result.FactorOverrides.BaseColor = Result.Factors.BaseColor;
			if (Factors.contains("Emissive"))
				Result.FactorOverrides.Emissive = Result.Factors.Emissive;
			if (Factors.contains("Metallic"))
				Result.FactorOverrides.Metallic = Result.Factors.Metallic;
			if (Factors.contains("Roughness"))
				Result.FactorOverrides.Roughness = Result.Factors.Roughness;
			if (Factors.contains("Specular"))
				Result.FactorOverrides.Specular = Result.Factors.Specular;
			if (Factors.contains("NormalScale"))
				Result.FactorOverrides.NormalScale = Result.Factors.NormalScale;
			if (Factors.contains("OcclusionStrength"))
				Result.FactorOverrides.OcclusionStrength = Result.Factors.OcclusionStrength;
			if (Factors.contains("AlphaCutoff"))
				Result.FactorOverrides.AlphaCutoff = Result.Factors.AlphaCutoff;
			if (Factors.contains("ClearCoat"))
				Result.FactorOverrides.ClearCoat = Result.Factors.ClearCoat;
			if (Factors.contains("ClearCoatRoughness"))
				Result.FactorOverrides.ClearCoatRoughness = Result.Factors.ClearCoatRoughness;
			if (Factors.contains("Transmission"))
				Result.FactorOverrides.Transmission = Result.Factors.Transmission;
			if (Factors.contains("IndexOfRefraction"))
				Result.FactorOverrides.IndexOfRefraction = Result.Factors.IndexOfRefraction;
		}
		if (StoredVersion != 1U && StoredVersion != MaterialDocument::CurrentFormatVersion)
			throw MaterialDocumentException("Material document format version is unsupported");
		if (Result.Type == MaterialDocumentType::MaterialInstance)
		{
			const Json Parent = Root.at("Parent");
			const string ParentType = Parent.value("AssetType", string("Material"));
			Result.Parent = MaterialParentReference{.ID = Parent.at("AssetID").get<string>(),
													.Type = ParentType == "Material" ? resource::AssetType::Material
																					 : resource::AssetType::MaterialInstance};
			if (ParentType != "Material" && ParentType != "MaterialInstance")
				throw MaterialDocumentException("Material instance parent type is invalid");
		}
		for (const Json &Texture : Root.value("Textures", Json::array()))
		{
			Result.Textures.push_back({.Semantic = ParseEnum(Texture.at("Semantic").get<string>(), TextureSemantics, "Texture Semantic"),
									   .ID = Texture.at("AssetID").get<string>(),
									   .TextureCoordinateChannel = Texture.value("TextureCoordinateChannel", 0U)});
		}
		if (Result.Type == MaterialDocumentType::MaterialInstance)
			Result.TextureOverrides = Result.Textures;
		MaterialDocumentStore::Validate(Result);
		return Result;
	}
	catch (const MaterialDocumentException &)
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		throw MaterialDocumentException("Could not load material document '" + Path.string() + "': " + Exception.what());
	}
}

void MaterialDocumentStore::Save(const MaterialDocument &Document, const std::filesystem::path &Path)
{
	MaterialDocumentStore::Validate(Document);
	const std::filesystem::path Destination = std::filesystem::absolute(Path.empty() ? Document.Path : Path).lexically_normal();
	if (Destination.empty())
		throw std::invalid_argument("Material save requires a destination path");
	const string ExpectedExtension = Document.Type == MaterialDocumentType::Material ? ".material" : ".materialinstance";
	if (Destination.extension() != ExpectedExtension)
		throw MaterialDocumentException("Material document extension does not match its type");
	Json Textures = Json::array();
	const std::vector<MaterialTextureReference> &AuthoredTextures =
		Document.Type == MaterialDocumentType::MaterialInstance ? Document.TextureOverrides : Document.Textures;
	for (const MaterialTextureReference &Texture : AuthoredTextures)
	{
		Textures.push_back({{"Semantic", EnumName(Texture.Semantic, TextureSemantics, "Texture Semantic")},
							{"AssetID", Texture.ID},
							{"TextureCoordinateChannel", Texture.TextureCoordinateChannel}});
	}
	Json FactorValues = Json::object();
	const auto StoreFactor = [&](const string_view Name, const auto &Value, const bool Overridden)
	{
		if (Document.Type == MaterialDocumentType::Material || Overridden)
			FactorValues[string(Name)] = Value;
	};
	StoreFactor("BaseColor",
				Json::array({Document.Factors.BaseColor.x, Document.Factors.BaseColor.y, Document.Factors.BaseColor.z,
							 Document.Factors.BaseColor.w}),
				Document.FactorOverrides.BaseColor.has_value());
	StoreFactor("Emissive", Json::array({Document.Factors.Emissive.x, Document.Factors.Emissive.y, Document.Factors.Emissive.z}),
				Document.FactorOverrides.Emissive.has_value());
	StoreFactor("Metallic", Document.Factors.Metallic, Document.FactorOverrides.Metallic.has_value());
	StoreFactor("Roughness", Document.Factors.Roughness, Document.FactorOverrides.Roughness.has_value());
	StoreFactor("Specular", Document.Factors.Specular, Document.FactorOverrides.Specular.has_value());
	StoreFactor("NormalScale", Document.Factors.NormalScale, Document.FactorOverrides.NormalScale.has_value());
	StoreFactor("OcclusionStrength", Document.Factors.OcclusionStrength, Document.FactorOverrides.OcclusionStrength.has_value());
	StoreFactor("AlphaCutoff", Document.Factors.AlphaCutoff, Document.FactorOverrides.AlphaCutoff.has_value());
	StoreFactor("ClearCoat", Document.Factors.ClearCoat, Document.FactorOverrides.ClearCoat.has_value());
	StoreFactor("ClearCoatRoughness", Document.Factors.ClearCoatRoughness, Document.FactorOverrides.ClearCoatRoughness.has_value());
	StoreFactor("Transmission", Document.Factors.Transmission, Document.FactorOverrides.Transmission.has_value());
	StoreFactor("IndexOfRefraction", Document.Factors.IndexOfRefraction, Document.FactorOverrides.IndexOfRefraction.has_value());
	Json Root{{"FormatVersion", MaterialDocument::CurrentFormatVersion},
			  {"DocumentID", Document.DocumentID},
			  {"AssetType", Document.Type == MaterialDocumentType::Material ? "Material" : "MaterialInstance"},
			  {"Name", Document.Name},
			  {"Factors", std::move(FactorValues)},
			  {"Textures", std::move(Textures)}};
	if (Document.Type == MaterialDocumentType::Material || Document.PipelineOverride.has_value())
	{
		Root["Pipeline"] = {{"ShadingModel", EnumName(Document.Pipeline.ShadingModel, ShadingModels, "ShadingModel")},
							{"BlendMode", EnumName(Document.Pipeline.BlendMode, BlendModes, "BlendMode")},
							{"TwoSided", Document.Pipeline.TwoSided},
							{"CastsShadows", Document.Pipeline.CastsShadows},
							{"ReceivesShadows", Document.Pipeline.ReceivesShadows}};
	}
	if (Document.Parent.has_value())
	{
		Root["Parent"] = {{"AssetType", Document.Parent->Type == resource::AssetType::Material ? "Material" : "MaterialInstance"},
						  {"AssetID", Document.Parent->ID}};
	}
	core::io::SecurePath::CreateTrustedRoot(Destination.parent_path(), "Material document root");
	const std::filesystem::path Temporary = Destination.filename().string() + "." + util::UUID::GenerateRandomUUID().ToString() + ".tmp";
	try
	{
		const string Serialized = Root.dump(2) + '\n';
		core::io::SecurePath::WriteFileWithin(Destination.parent_path(), Temporary,
											  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), false, true,
											  "Material document temporary file");
		core::io::SecurePath::ReplaceWithin(Destination.parent_path(), Temporary, Destination.filename(), "Material document publication");
	}
	catch (...)
	{
		try
		{
			core::io::SecurePath::RemoveWithin(Destination.parent_path(), Temporary, false, "Material document cleanup");
		}
		catch (...)
		{
		}
		throw;
	}
}

void MaterialDocumentStore::Validate(const MaterialDocument &Document)
{
	if (Document.FormatVersion != MaterialDocument::CurrentFormatVersion)
		throw MaterialDocumentException("Material document format version is unsupported");
	if (Document.Name.empty())
		throw MaterialDocumentException("Material document requires a name");
	if (!util::UUID::TryParse(Document.DocumentID).has_value())
		throw MaterialDocumentException("Material document requires a stable UUID identity");
	const auto Finite = [](const float32 Value) { return std::isfinite(Value); };
	const resource::PBRMaterialFactors &Factors = Document.Factors;
	const std::array Values{Factors.BaseColor.x,	  Factors.BaseColor.y, Factors.BaseColor.z,		   Factors.BaseColor.w,
							Factors.Emissive.x,		  Factors.Emissive.y,  Factors.Emissive.z,		   Factors.Metallic,
							Factors.Roughness,		  Factors.Specular,	   Factors.NormalScale,		   Factors.OcclusionStrength,
							Factors.AlphaCutoff,	  Factors.ClearCoat,   Factors.ClearCoatRoughness, Factors.Transmission,
							Factors.IndexOfRefraction};
	if (!std::ranges::all_of(Values, Finite))
		throw MaterialDocumentException("Material factors must be finite");
	const auto Unit = [](const float32 Value) { return Value >= 0.0f && Value <= 1.0f; };
	if (!Unit(Factors.BaseColor.x) || !Unit(Factors.BaseColor.y) || !Unit(Factors.BaseColor.z) || !Unit(Factors.BaseColor.w) ||
		Factors.Emissive.x < 0.0f || Factors.Emissive.y < 0.0f || Factors.Emissive.z < 0.0f || !Unit(Factors.Metallic) ||
		!Unit(Factors.Roughness) || !Unit(Factors.Specular) || !Unit(Factors.AlphaCutoff) || !Unit(Factors.ClearCoat) ||
		!Unit(Factors.ClearCoatRoughness) || !Unit(Factors.Transmission) || Factors.NormalScale < 0.0f || Factors.NormalScale > 8.0f ||
		!Unit(Factors.OcclusionStrength) || Factors.IndexOfRefraction < 1.0f || Factors.IndexOfRefraction > 4.0f)
	{
		throw MaterialDocumentException("Material factors are outside their valid physical range");
	}
	if (Document.Type == MaterialDocumentType::Material && Document.Parent.has_value())
		throw MaterialDocumentException("Base material cannot have a parent");
	if (Factors.Transmission > 0.0f && Document.Pipeline.BlendMode != resource::MaterialBlendMode::Translucent)
		throw MaterialDocumentException("Material transmission requires the translucent blend mode");
	if (Factors.ClearCoat > 0.0f && Document.Pipeline.ShadingModel != resource::MaterialShadingModel::ClearCoat)
		throw MaterialDocumentException("Clear-coat factors require the clear-coat shading model");
	if (Document.Type == MaterialDocumentType::MaterialInstance &&
		(!Document.Parent.has_value() || Document.Parent->ID.empty() ||
		 (Document.Parent->Type != resource::AssetType::Material && Document.Parent->Type != resource::AssetType::MaterialInstance)))
	{
		throw MaterialDocumentException("Material instance requires a canonical material parent identity and type");
	}
	std::unordered_set<resource::MaterialTextureSemantic> Semantics;
	for (const MaterialTextureReference &Texture : Document.Textures)
	{
		if (Texture.ID.empty())
			throw MaterialDocumentException("Material texture reference requires a canonical asset identity");
		if (Texture.TextureCoordinateChannel >= resource::MaterialTextureCoordinateChannelCount)
			throw MaterialDocumentException("Material texture coordinate channel is unsupported");
		if (!Semantics.insert(Texture.Semantic).second)
			throw MaterialDocumentException("Material texture semantics must be unique");
	}
}

MaterialDocument MaterialDocumentStore::Resolve(const MaterialDocument &Document, resource::AssetManager &Assets)
{
	MaterialDocumentStore::Validate(Document);
	std::vector<MaterialDocument> Chain{Document};
	std::unordered_set<resource::AssetID> Visited{Document.DocumentID};
	while (Chain.back().Type == MaterialDocumentType::MaterialInstance)
	{
		if (Chain.size() >= 64U)
			throw MaterialDocumentException("Material parent chain exceeds the engine depth limit");
		const MaterialParentReference &Parent = *Chain.back().Parent;
		if (!Visited.insert(Parent.ID).second)
			throw MaterialDocumentException("Material parent chain contains a cycle");
		const std::optional<resource::AssetRecordSnapshot> Record = Assets.SnapshotRecord(Parent.ID);
		if (!Record.has_value() || Record->Type != Parent.Type ||
			(Parent.Type != resource::AssetType::Material && Parent.Type != resource::AssetType::MaterialInstance))
		{
			throw MaterialDocumentException("Material parent does not resolve to its declared asset identity and type");
		}
		MaterialDocument ParentDocument = MaterialDocumentStore::Load(Record->CanonicalPath);
		const MaterialDocumentType Expected =
			Parent.Type == resource::AssetType::Material ? MaterialDocumentType::Material : MaterialDocumentType::MaterialInstance;
		if (ParentDocument.Type != Expected)
			throw MaterialDocumentException("Material parent document type does not match its asset record");
		Chain.push_back(std::move(ParentDocument));
	}

	MaterialDocument Resolved = Chain.back();
	for (auto Iterator = Chain.rbegin() + 1; Iterator != Chain.rend(); ++Iterator)
	{
		if (Iterator->PipelineOverride)
		{
			Resolved.Pipeline = *Iterator->PipelineOverride;
			if (Resolved.Pipeline.ShadingModel != resource::MaterialShadingModel::ClearCoat && !Iterator->FactorOverrides.ClearCoat)
			{
				Resolved.Factors.ClearCoat = 0.0f;
				if (!Iterator->FactorOverrides.ClearCoatRoughness)
					Resolved.Factors.ClearCoatRoughness = 0.0f;
			}
			if (Resolved.Pipeline.BlendMode != resource::MaterialBlendMode::Translucent && !Iterator->FactorOverrides.Transmission)
				Resolved.Factors.Transmission = 0.0f;
		}
		Iterator->FactorOverrides.Apply(Resolved.Factors);
		for (const MaterialTextureReference &Texture : Iterator->Textures)
		{
			const auto Existing = std::ranges::find(Resolved.Textures, Texture.Semantic, &MaterialTextureReference::Semantic);
			if (Existing == Resolved.Textures.end())
				Resolved.Textures.push_back(Texture);
			else
				*Existing = Texture;
		}
	}
	Resolved.DocumentID = Document.DocumentID;
	Resolved.Name = Document.Name;
	Resolved.Path = Document.Path;
	Resolved.Type = Document.Type;
	Resolved.Parent = Document.Parent;
	Resolved.FactorOverrides = Document.FactorOverrides;
	Resolved.PipelineOverride = Document.PipelineOverride;
	Resolved.TextureOverrides = Document.Textures;
	for (const MaterialTextureReference &Texture : Resolved.Textures)
	{
		const std::optional<resource::AssetRecordSnapshot> Record = Assets.SnapshotRecord(Texture.ID);
		if (!Record.has_value() || Record->Type != resource::AssetType::Texture2D)
			throw MaterialDocumentException("Material texture dependency does not resolve to a Texture2D asset");
	}
	return Resolved;
}

MaterialPreviewResource::~MaterialPreviewResource()
{
	this->Retire();
}
void MaterialPreviewResource::Request(const uint64 Revision)
{
	this->Retire();
	std::scoped_lock Lock(this->Mutex);
	this->Cancellation = std::stop_source{};
	this->Current = {.Revision = Revision, .State = MaterialPreviewState::Requested};
}
std::stop_token MaterialPreviewResource::BeginRealization(const uint64 Revision)
{
	std::scoped_lock Lock(this->Mutex);
	if (this->Current.Revision != Revision || this->Current.State != MaterialPreviewState::Requested)
		throw MaterialDocumentException("Material preview realization is stale or was not requested");
	this->Current.State = MaterialPreviewState::Realizing;
	return this->Cancellation.get_token();
}
void MaterialPreviewResource::Publish(const uint64 Revision, const uint64 TextureHandle, std::function<void(uint64)> QueueRetirement)
{
	if (TextureHandle == 0 || !QueueRetirement)
		throw MaterialDocumentException("Material preview publication requires a GPU texture and retirement queue");
	bool Stale = false;
	{
		std::scoped_lock Lock(this->Mutex);
		Stale = this->Current.Revision != Revision || this->Current.State != MaterialPreviewState::Realizing ||
				this->Cancellation.stop_requested();
		if (!Stale)
		{
			this->QueueRetirement = QueueRetirement;
			this->Current.TextureHandle = TextureHandle;
			this->Current.State = MaterialPreviewState::Ready;
			this->Current.Diagnostic.clear();
		}
	}
	if (Stale)
		QueueRetirement(TextureHandle);
}
void MaterialPreviewResource::Fail(const uint64 Revision, string Diagnostic)
{
	std::scoped_lock Lock(this->Mutex);
	if (this->Current.Revision != Revision)
		return;
	this->Current.State = MaterialPreviewState::Failed;
	this->Current.Diagnostic = std::move(Diagnostic);
}
void MaterialPreviewResource::Retire() noexcept
{
	uint64 TextureHandle = 0;
	std::function<void(uint64)> Retirement;
	{
		std::scoped_lock Lock(this->Mutex);
		this->Cancellation.request_stop();
		TextureHandle = std::exchange(this->Current.TextureHandle, 0);
		Retirement = std::move(this->QueueRetirement);
		if (TextureHandle != 0)
			this->Current.State = MaterialPreviewState::RetirementPending;
	}
	if (TextureHandle != 0 && Retirement)
	{
		try
		{
			Retirement(TextureHandle);
		}
		catch (...)
		{
		}
	}
}
MaterialPreviewSnapshot MaterialPreviewResource::Snapshot() const
{
	std::scoped_lock Lock(this->Mutex);
	return this->Current;
}

MaterialEditorSession MaterialEditorSession::Open(const std::filesystem::path &Path, resource::AssetManager &Assets)
{
	MaterialEditorSession Session;
	Session.Assets = &Assets;
	const MaterialDocument Authored = MaterialDocumentStore::Load(Path);
	Session.Document = MaterialDocumentStore::Resolve(Authored, Assets);
	Session.Preview->Request(0);
	std::error_code Error;
	Session.BaselineWriteTime = std::filesystem::last_write_time(Session.Document.Path, Error);
	if (Error)
		throw MaterialDocumentException("Could not capture material document revision: " + Error.message());
	return Session;
}

const MaterialDocument &MaterialEditorSession::GetDocument() const noexcept
{
	return this->Document;
}
MaterialDocument &MaterialEditorSession::Edit()
{
	return this->Document;
}
void MaterialEditorSession::CommitEdit(MaterialDocument Before)
{
	MaterialDocumentStore::Validate(this->Document);
	if (this->Document.Type == MaterialDocumentType::MaterialInstance)
	{
		for (const MaterialTextureReference &Texture : this->Document.Textures)
		{
			const auto Previous = std::ranges::find(Before.Textures, Texture.Semantic, &MaterialTextureReference::Semantic);
			if (Previous != Before.Textures.end() && Previous->ID == Texture.ID &&
				Previous->TextureCoordinateChannel == Texture.TextureCoordinateChannel)
				continue;
			const auto Existing = std::ranges::find(this->Document.TextureOverrides, Texture.Semantic, &MaterialTextureReference::Semantic);
			if (Existing == this->Document.TextureOverrides.end())
				this->Document.TextureOverrides.push_back(Texture);
			else
				*Existing = Texture;
		}
	}
	this->UndoStack.push_back(std::move(Before));
	this->Dirty = true;
	++this->Revision;
	this->CancelPreviewTasks();
	this->Preview->Request(this->Revision);
}
void MaterialEditorSession::BeginEditGesture(MaterialDocument Before)
{
	if (!this->ActiveGestureBefore)
		this->ActiveGestureBefore = std::move(Before);
}
void MaterialEditorSession::EndEditGesture()
{
	if (!this->ActiveGestureBefore)
		return;
	MaterialDocument Before = std::move(*this->ActiveGestureBefore);
	this->ActiveGestureBefore.reset();
	this->CommitEdit(std::move(Before));
}
bool MaterialEditorSession::HasActiveEditGesture() const noexcept
{
	return this->ActiveGestureBefore.has_value();
}
bool MaterialEditorSession::IsDirty() const noexcept
{
	return this->Dirty;
}
bool MaterialEditorSession::HasReloadConflict() const noexcept
{
	return this->ReloadConflict;
}
void MaterialEditorSession::Save()
{
	this->EndEditGesture();
	std::error_code Error;
	const auto CurrentWriteTime = std::filesystem::last_write_time(this->Document.Path, Error);
	if (!Error && CurrentWriteTime != this->BaselineWriteTime)
	{
		this->ReloadConflict = true;
		throw MaterialDocumentException("Material document changed on disk; explicit reload conflict resolution is required");
	}
	MaterialDocumentStore::Save(this->Document);
	this->BaselineWriteTime = std::filesystem::last_write_time(this->Document.Path);
	this->BaselineRevision = this->Revision;
	this->Dirty = false;
	this->ReloadConflict = false;
}
void MaterialEditorSession::Reload(const bool DiscardChanges)
{
	this->ActiveGestureBefore.reset();
	if (this->Dirty && !DiscardChanges)
	{
		this->ReloadConflict = true;
		throw MaterialDocumentException("Reload would discard unsaved material edits");
	}
	this->CancelPreviewTasks();
	const MaterialDocument Authored = MaterialDocumentStore::Load(this->Document.Path);
	if (this->Assets == nullptr)
		throw MaterialDocumentException("Material editor session lost its asset resolver");
	this->Document = MaterialDocumentStore::Resolve(Authored, *this->Assets);
	this->BaselineWriteTime = std::filesystem::last_write_time(this->Document.Path);
	this->UndoStack.clear();
	this->Dirty = false;
	this->ReloadConflict = false;
	this->BaselineRevision = ++this->Revision;
	this->Preview->Request(this->Revision);
}
bool MaterialEditorSession::Undo()
{
	if (this->UndoStack.empty())
		return false;
	this->CancelPreviewTasks();
	this->Document = std::move(this->UndoStack.back());
	this->UndoStack.pop_back();
	this->Dirty = this->Revision != this->BaselineRevision;
	if (this->Revision > 0)
		--this->Revision;
	this->Preview->Request(this->Revision);
	return true;
}
uint64 MaterialEditorSession::GetPreviewRevision() const noexcept
{
	return this->Revision;
}
std::stop_token MaterialEditorSession::BeginPreviewTask()
{
	this->PreviewStop.request_stop();
	this->PreviewStop = std::stop_source{};
	return this->PreviewStop.get_token();
}
void MaterialEditorSession::CancelPreviewTasks() noexcept
{
	this->PreviewStop.request_stop();
}
std::shared_ptr<MaterialPreviewResource> MaterialEditorSession::GetPreviewResource() const noexcept
{
	return this->Preview;
}
} // namespace editor::material
