#include "ModelAssetImporter.h"

#include "src/concepts.h"
#include "src/resource/asset/AnimationAsset.h"
#include "src/resource/asset/MaterialAsset.h"
#include "src/resource/asset/MeshAsset.h"
#include "src/resource/asset/ModelAsset.h"
#include "src/resource/asset/SkeletonAsset.h"
#include "src/resource/asset/Texture2DAsset.h"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <assimp/Importer.hpp>
#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace resource::importer
{
namespace
{
[[nodiscard]] uint64 StableHash(string_view Text)
{
	uint64 Hash = 1469598103934665603ULL;
	for (const uint8 Character : Text)
	{
		Hash ^= static_cast<uint8>(Character);
		Hash *= 1099511628211ULL;
	}
	return Hash == 0 ? 1 : Hash;
}

void ExtendHash(uint64 &Hash, std::span<const uint8> Bytes) noexcept
{
	for (const uint8 Value : Bytes)
	{
		Hash ^= Value;
		Hash *= 1099511628211ULL;
	}
}

template <TriviallyCopyable T> void ExtendHashValue(uint64 &Hash, const T &Value) noexcept
{
	ExtendHash(Hash, std::span<const uint8>(reinterpret_cast<const uint8 *>(&Value), sizeof(T)));
}

[[nodiscard]] string BuildMeshDerivedDataKey(const std::filesystem::path &Source, uint32 MeshIndex, const MeshAssetData &Data)
{
	uint64 Hash = StableHash("MeshDerivedData:v2:" + Source.generic_string());
	ExtendHashValue(Hash, MeshIndex);
	for (const MeshLOD &LOD : Data.LODs)
	{
		ExtendHashValue(Hash, LOD.Level);
		ExtendHashValue(Hash, LOD.ScreenCoverage);
		for (const MeshVertexStream &Stream : LOD.VertexStreams)
		{
			ExtendHashValue(Hash, Stream.Semantic);
			ExtendHashValue(Hash, Stream.Format);
			ExtendHashValue(Hash, Stream.SemanticIndex);
			ExtendHashValue(Hash, Stream.Stride);
			ExtendHash(Hash, Stream.Bytes);
		}
		ExtendHashValue(Hash, LOD.IndexStream.Format);
		ExtendHash(Hash, LOD.IndexStream.Bytes);
		for (const MorphTargetLODData &Morph : LOD.MorphTargets)
		{
			ExtendHashValue(Hash, Morph.Target);
			ExtendHash(Hash, Morph.PositionDeltas.Bytes);
			ExtendHash(Hash, Morph.NormalDeltas.Bytes);
		}
	}
	return std::to_string(Hash == 0 ? 1 : Hash);
}

[[nodiscard]] std::filesystem::path SubassetPath(const std::filesystem::path &Source, string_view Category, uint32 Index)
{
	return std::filesystem::path(Source.generic_string() + "#" + string(Category) + "/" + std::to_string(Index));
}

[[nodiscard]] glm::mat4 ToMatrix(const aiMatrix4x4 &Value)
{
	return glm::mat4(Value.a1, Value.b1, Value.c1, Value.d1, Value.a2, Value.b2, Value.c2, Value.d2, Value.a3, Value.b3, Value.c3, Value.d3,
					 Value.a4, Value.b4, Value.c4, Value.d4);
}

template <TriviallyCopyable T> [[nodiscard]] std::vector<uint8> ToBytes(const std::vector<T> &Values)
{
	if (Values.size() > std::numeric_limits<usize>::max() / sizeof(T))
	{
		throw std::overflow_error("Imported mesh stream exceeds addressable memory");
	}
	std::vector<uint8> Bytes(Values.size() * sizeof(T));
	if (!Bytes.empty())
		std::memcpy(Bytes.data(), Values.data(), Bytes.size());
	return Bytes;
}

[[nodiscard]] Bounds CalculateBounds(std::span<const glm::vec3> Positions)
{
	if (Positions.empty())
		throw std::invalid_argument("Cannot calculate bounds for empty geometry");
	Bounds Bounds;
	Bounds.Minimum = Positions.front();
	Bounds.Maximum = Positions.front();
	for (const glm::vec3 &Position : Positions)
	{
		if (!std::isfinite(Position.x) || !std::isfinite(Position.y) || !std::isfinite(Position.z))
		{
			throw std::invalid_argument("Imported mesh contains non-finite positions");
		}
		Bounds.Minimum = glm::min(Bounds.Minimum, Position);
		Bounds.Maximum = glm::max(Bounds.Maximum, Position);
	}
	const glm::vec3 Center = (Bounds.Minimum + Bounds.Maximum) * 0.5f;
	float32 Radius = 0.0f;
	for (const glm::vec3 &Position : Positions)
		Radius = std::max(Radius, glm::length(Position - Center));
	Bounds.Sphere = glm::vec4(Center, Radius);
	return Bounds;
}

[[nodiscard]] Bounds CalculateBoundsFromExtents(const glm::vec3 &Minimum, const glm::vec3 &Maximum)
{
	Bounds Result{.Minimum = Minimum, .Maximum = Maximum};
	const glm::vec3 Center = (Minimum + Maximum) * 0.5f;
	Result.Sphere = glm::vec4(Center, glm::length(Maximum - Center));
	if (!Result.IsValid())
		throw std::invalid_argument("Cannot calculate bounds from invalid deformation extents");
	return Result;
}

void IncludeTransformedBounds(Bounds &Destination, bool &Initialized, const Bounds &Source, const glm::mat4 &Transform)
{
	for (uint32 Corner = 0; Corner < 8; ++Corner)
	{
		const glm::vec3 Local{(Corner & 1U) != 0 ? Source.Maximum.x : Source.Minimum.x,
							  (Corner & 2U) != 0 ? Source.Maximum.y : Source.Minimum.y,
							  (Corner & 4U) != 0 ? Source.Maximum.z : Source.Minimum.z};
		const glm::vec3 Position = glm::vec3(Transform * glm::vec4(Local, 1.0f));
		if (!Initialized)
		{
			Destination.Minimum = Position;
			Destination.Maximum = Position;
			Initialized = true;
		}
		else
		{
			Destination.Minimum = glm::min(Destination.Minimum, Position);
			Destination.Maximum = glm::max(Destination.Maximum, Position);
		}
	}
	const glm::vec3 Center = (Destination.Minimum + Destination.Maximum) * 0.5f;
	Destination.Sphere = glm::vec4(Center, glm::length(Destination.Maximum - Center));
}

[[nodiscard]] AssetHandle<Texture2DAsset> ImportEmbeddedTexture(uint32 TextureIndex, const aiScene &Scene,
																const std::filesystem::path &ModelPath, AssetImportContext &Context,
																std::unordered_map<uint32, AssetHandle<Texture2DAsset>> &ImportedTextures)
{
	const auto Existing = ImportedTextures.find(TextureIndex);
	if (Existing != ImportedTextures.end())
		return Existing->second;
	if (Scene.mTextures == nullptr || TextureIndex >= Scene.mNumTextures || Scene.mTextures[TextureIndex] == nullptr)
	{
		throw AssetContentValidationException(AssetType::Model, ModelPath, "Material references an invalid embedded texture");
	}
	const aiTexture &Source = *Scene.mTextures[TextureIndex];
	int32 Width = 0;
	int32 Height = 0;
	constexpr int32 ChannelCount = 4;
	std::vector<uint8> Pixels;
	if (Source.mHeight == 0)
	{
		if (Source.mWidth == 0 || Source.pcData == nullptr || Source.mWidth > static_cast<uint32>(std::numeric_limits<int32>::max()))
		{
			throw AssetImageDecodeException(AssetType::Model, ModelPath, "Embedded compressed texture payload is invalid");
		}
		int32 DecodedChannels = 0;
		stbi_uc *Decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(Source.pcData), static_cast<int32>(Source.mWidth),
												 &Width, &Height, &DecodedChannels, STBI_rgb_alpha);
		if (Decoded == nullptr)
		{
			const auto *Reason = stbi_failure_reason();
			throw AssetImageDecodeException(AssetType::Model, ModelPath,
											Reason == nullptr ? "Embedded image decoder did not provide a diagnostic" : Reason);
		}
		std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> Owned(Decoded, stbi_image_free);
		if (Width <= 0 || Height <= 0 ||
			static_cast<uint64>(Width) * static_cast<uint64>(Height) >
				std::numeric_limits<usize>::max() / static_cast<uint64>(ChannelCount))
		{
			throw AssetContentValidationException(AssetType::Model, ModelPath,
												  "Embedded texture dimensions are invalid or overflow memory");
		}
		const usize ByteCount = static_cast<usize>(Width) * static_cast<usize>(Height) * static_cast<usize>(ChannelCount);
		Pixels.assign(Owned.get(), Owned.get() + ByteCount);
	}
	else
	{
		if (Source.pcData == nullptr || Source.mWidth > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
			Source.mHeight > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
			static_cast<uint64>(Source.mWidth) * Source.mHeight > std::numeric_limits<usize>::max() / static_cast<uint64>(ChannelCount))
		{
			throw AssetContentValidationException(AssetType::Model, ModelPath,
												  "Embedded raw texture dimensions are invalid or overflow memory");
		}
		Width = static_cast<int32>(Source.mWidth);
		Height = static_cast<int32>(Source.mHeight);
		Pixels.resize(static_cast<usize>(Width) * static_cast<usize>(Height) * static_cast<usize>(ChannelCount));
		for (usize PixelIndex = 0; PixelIndex < static_cast<usize>(Width) * static_cast<usize>(Height); ++PixelIndex)
		{
			const aiTexel &Texel = Source.pcData[PixelIndex];
			Pixels[PixelIndex * 4U + 0U] = Texel.r;
			Pixels[PixelIndex * 4U + 1U] = Texel.g;
			Pixels[PixelIndex * 4U + 2U] = Texel.b;
			Pixels[PixelIndex * 4U + 3U] = Texel.a;
		}
	}

	const std::filesystem::path TexturePath = SubassetPath(ModelPath, "embedded-texture", TextureIndex);
	auto Handle = Context.Reserve<Texture2DAsset>(AssetType::Texture2D, TexturePath);
	Context.Stage(
		AssetType::Texture2D, TexturePath,
		AssetPtr<Texture2DAsset>::Make("EmbeddedTexture_" + std::to_string(TextureIndex), Width, Height, ChannelCount, std::move(Pixels)));
	ImportedTextures.emplace(TextureIndex, Handle);
	return Handle;
}

void AppendTexture(const aiMaterial &Material, aiTextureType SourceSemantic, MaterialTextureSemantic DestinationSemantic,
				   const aiScene &Scene, const std::filesystem::path &ModelPath, AssetImportContext &Context,
				   std::unordered_map<uint32, AssetHandle<Texture2DAsset>> &EmbeddedTextures, std::vector<MaterialTextureBinding> &Bindings,
				   std::vector<AssetDependency> &Dependencies)
{
	aiString ImportedPath;
	uint32 TextureCoordinateChannel = 0;
	if (Material.GetTexture(SourceSemantic, 0, &ImportedPath, nullptr, &TextureCoordinateChannel) != AI_SUCCESS)
		return;
	if (TextureCoordinateChannel >= MaterialTextureCoordinateChannelCount)
	{
		throw AssetContentValidationException(AssetType::Model, ModelPath,
											  "Material texture references TEXCOORD_" + std::to_string(TextureCoordinateChannel) +
												  ", but the renderer supports TEXCOORD_0 through TEXCOORD_" +
												  std::to_string(MaterialTextureCoordinateChannelCount - 1U));
	}
	const string PathText = ImportedPath.C_Str();
	if (!PathText.empty() && PathText.front() == '*')
	{
		uint32 EmbeddedIndex = 0;
		const auto Parse = std::from_chars(PathText.data() + 1, PathText.data() + PathText.size(), EmbeddedIndex);
		if (Parse.ec != std::errc{} || Parse.ptr != PathText.data() + PathText.size())
		{
			throw AssetContentValidationException(AssetType::Model, ModelPath, "Embedded texture reference is malformed: " + PathText);
		}
		auto Texture = ImportEmbeddedTexture(EmbeddedIndex, Scene, ModelPath, Context, EmbeddedTextures);
		const std::filesystem::path TexturePath = SubassetPath(ModelPath, "embedded-texture", EmbeddedIndex);
		Bindings.push_back({DestinationSemantic, std::move(Texture), TextureCoordinateChannel});
		Dependencies.push_back({AssetType::Texture2D, TexturePath});
		return;
	}
	const std::filesystem::path TexturePath = (ModelPath.parent_path() / PathText).lexically_normal();
	auto Texture = Context.Reserve<Texture2DAsset>(AssetType::Texture2D, TexturePath);
	Bindings.push_back({DestinationSemantic, std::move(Texture), TextureCoordinateChannel});
	Dependencies.push_back({AssetType::Texture2D, TexturePath});
}

[[nodiscard]] AssetHandle<MaterialAsset> ImportMaterial(const aiMaterial &Material, uint32 MaterialIndex, const aiScene &Scene,
														const std::filesystem::path &ModelPath, AssetImportContext &Context,
														std::unordered_map<uint32, AssetHandle<Texture2DAsset>> &EmbeddedTextures,
														std::vector<AssetDependency> &RootDependencies)
{
	aiString ImportedName;
	(void)Material.Get(AI_MATKEY_NAME, ImportedName);
	string Name = ImportedName.length == 0 ? "Material_" + std::to_string(MaterialIndex) : ImportedName.C_Str();

	PBRMaterialFactors Factors;
	aiColor4D BaseColor;
	if (aiGetMaterialColor(&Material, AI_MATKEY_BASE_COLOR, &BaseColor) == AI_SUCCESS ||
		aiGetMaterialColor(&Material, AI_MATKEY_COLOR_DIFFUSE, &BaseColor) == AI_SUCCESS)
	{
		Factors.BaseColor = {BaseColor.r, BaseColor.g, BaseColor.b, BaseColor.a};
	}
	(void)Material.Get(AI_MATKEY_METALLIC_FACTOR, Factors.Metallic);
	if (Material.Get(AI_MATKEY_ROUGHNESS_FACTOR, Factors.Roughness) != AI_SUCCESS)
	{
		float32 Shininess = 0.0f;
		if (Material.Get(AI_MATKEY_SHININESS, Shininess) == AI_SUCCESS && std::isfinite(Shininess) && Shininess >= 0.0f)
			Factors.Roughness = std::clamp(std::sqrt(2.0f / (Shininess + 2.0f)), 0.045f, 1.0f);
	}
	aiColor3D SpecularColor;
	if (Material.Get(AI_MATKEY_COLOR_SPECULAR, SpecularColor) == AI_SUCCESS)
		Factors.Specular = std::clamp(std::max({SpecularColor.r, SpecularColor.g, SpecularColor.b}), 0.0f, 1.0f);
	aiColor3D Emissive;
	if (Material.Get(AI_MATKEY_COLOR_EMISSIVE, Emissive) == AI_SUCCESS)
	{
		Factors.Emissive = {Emissive.r, Emissive.g, Emissive.b};
	}
	(void)Material.Get(AI_MATKEY_OPACITY, Factors.BaseColor.a);
	MaterialPipelineContract PipelineContract;
	int32 TwoSided = 0;
	if (Material.Get(AI_MATKEY_TWOSIDED, TwoSided) == AI_SUCCESS)
		PipelineContract.TwoSided = TwoSided != 0;
	aiString AlphaMode;
	if (Material.Get(AI_MATKEY_GLTF_ALPHAMODE, AlphaMode) == AI_SUCCESS)
	{
		const string Mode = AlphaMode.C_Str();
		if (Mode == "OPAQUE")
			PipelineContract.BlendMode = MaterialBlendMode::Opaque;
		else if (Mode == "MASK")
		{
			PipelineContract.BlendMode = MaterialBlendMode::Masked;
			(void)Material.Get(AI_MATKEY_GLTF_ALPHACUTOFF, Factors.AlphaCutoff);
		}
		else if (Mode == "BLEND")
			PipelineContract.BlendMode = MaterialBlendMode::Translucent;
		else
			throw AssetContentValidationException(AssetType::Model, ModelPath, "Material has an invalid glTF alpha mode: " + Mode);
	}
	else if (Factors.BaseColor.a < 1.0f)
	{
		PipelineContract.BlendMode = MaterialBlendMode::Translucent;
	}

	std::vector<MaterialTextureBinding> Bindings;
	std::vector<AssetDependency> MaterialDependencies;
	AppendTexture(Material, aiTextureType_BASE_COLOR, MaterialTextureSemantic::BaseColor, Scene, ModelPath, Context, EmbeddedTextures,
				  Bindings, MaterialDependencies);
	if (Bindings.empty())
		AppendTexture(Material, aiTextureType_DIFFUSE, MaterialTextureSemantic::BaseColor, Scene, ModelPath, Context, EmbeddedTextures,
					  Bindings, MaterialDependencies);
	AppendTexture(Material, aiTextureType_NORMALS, MaterialTextureSemantic::Normal, Scene, ModelPath, Context, EmbeddedTextures, Bindings,
				  MaterialDependencies);
	if (std::none_of(Bindings.begin(), Bindings.end(),
					 [](const MaterialTextureBinding &Binding) { return Binding.Semantic == MaterialTextureSemantic::Normal; }))
	{
		AppendTexture(Material, aiTextureType_HEIGHT, MaterialTextureSemantic::Normal, Scene, ModelPath, Context, EmbeddedTextures,
					  Bindings, MaterialDependencies);
	}
	AppendTexture(Material, aiTextureType_METALNESS, MaterialTextureSemantic::MetallicRoughness, Scene, ModelPath, Context,
				  EmbeddedTextures, Bindings, MaterialDependencies);
	AppendTexture(Material, aiTextureType_AMBIENT_OCCLUSION, MaterialTextureSemantic::Occlusion, Scene, ModelPath, Context,
				  EmbeddedTextures, Bindings, MaterialDependencies);
	AppendTexture(Material, aiTextureType_EMISSIVE, MaterialTextureSemantic::Emissive, Scene, ModelPath, Context, EmbeddedTextures,
				  Bindings, MaterialDependencies);
	AppendTexture(Material, aiTextureType_SPECULAR, MaterialTextureSemantic::Specular, Scene, ModelPath, Context, EmbeddedTextures,
				  Bindings, MaterialDependencies);

	const std::filesystem::path MaterialPath = SubassetPath(ModelPath, "material", MaterialIndex);
	auto Handle = Context.Reserve<MaterialAsset>(AssetType::Material, MaterialPath);
	Context.Stage(AssetType::Material, MaterialPath,
				  AssetPtr<MaterialAsset>::Make(std::move(Name), PipelineContract, Factors, std::move(Bindings)), MaterialDependencies);
	RootDependencies.push_back({AssetType::Material, MaterialPath});
	return Handle;
}

struct ImportedMesh final
{
	AssetHandle<MeshAsset> Handle;
	AssetType Type = AssetType::StaticMesh;
	Bounds Bounds;
	std::filesystem::path Path;
};

struct ImportedSkeleton final
{
	AssetHandle<SkeletonAsset> Handle;
	std::filesystem::path Path;
	std::vector<SkeletonJoint> Joints;
	std::unordered_map<string, uint32> JointIndices;
};

void IndexNodes(const aiNode &Node, std::unordered_map<string, const aiNode *> &Nodes)
{
	const string Name = Node.mName.C_Str();
	if (!Name.empty() && !Nodes.emplace(Name, &Node).second)
	{
		throw std::invalid_argument("Model hierarchy contains duplicate node names required for skeletal binding");
	}
	for (uint32 ChildIndex = 0; ChildIndex < Node.mNumChildren; ++ChildIndex)
	{
		if (Node.mChildren[ChildIndex] == nullptr)
			throw std::invalid_argument("Model hierarchy contains a null child node");
		IndexNodes(*Node.mChildren[ChildIndex], Nodes);
	}
}

void AppendSkeletonJoints(const aiNode &Node, uint32 ParentJoint, const std::unordered_set<string> &RequiredNodes,
						  const std::unordered_map<string, aiMatrix4x4> &InverseBinds, const std::filesystem::path &ModelPath,
						  ImportedSkeleton &Result)
{
	const string Name = Node.mName.C_Str();
	uint32 NextParent = ParentJoint;
	if (RequiredNodes.contains(Name))
	{
		NextParent = static_cast<uint32>(Result.Joints.size());
		const auto InverseBind = InverseBinds.find(Name);
		Result.Joints.push_back({.ID = StableHash(ModelPath.generic_string() + ":joint:" + Name),
								 .Name = Name,
								 .ParentIndex = ParentJoint,
								 .ReferenceLocalTransform = ToMatrix(Node.mTransformation),
								 .InverseBindMatrix = InverseBind == InverseBinds.end() ? glm::mat4(1.0f) : ToMatrix(InverseBind->second)});
		Result.JointIndices.emplace(Name, NextParent);
	}
	for (uint32 ChildIndex = 0; ChildIndex < Node.mNumChildren; ++ChildIndex)
	{
		AppendSkeletonJoints(*Node.mChildren[ChildIndex], NextParent, RequiredNodes, InverseBinds, ModelPath, Result);
	}
}

[[nodiscard]] ImportedSkeleton ImportSkeleton(const aiScene &Scene, const std::filesystem::path &ModelPath, AssetImportContext &Context,
											  std::vector<AssetDependency> &RootDependencies)
{
	std::unordered_map<string, aiMatrix4x4> InverseBinds;
	for (uint32 MeshIndex = 0; MeshIndex < Scene.mNumMeshes; ++MeshIndex)
	{
		const aiMesh &Mesh = *Scene.mMeshes[MeshIndex];
		for (uint32 BoneIndex = 0; BoneIndex < Mesh.mNumBones; ++BoneIndex)
		{
			if (Mesh.mBones == nullptr || Mesh.mBones[BoneIndex] == nullptr)
			{
				throw AssetContentValidationException(AssetType::Model, ModelPath, "Skeletal mesh contains a null bone");
			}
			const aiBone &Bone = *Mesh.mBones[BoneIndex];
			const string Name = Bone.mName.C_Str();
			if (Name.empty())
				throw AssetContentValidationException(AssetType::Model, ModelPath, "Skeletal mesh contains an unnamed bone");
			const auto [Existing, Inserted] = InverseBinds.try_emplace(Name, Bone.mOffsetMatrix);
			if (!Inserted)
			{
				const glm::mat4 ExistingMatrix = ToMatrix(Existing->second);
				const glm::mat4 CandidateMatrix = ToMatrix(Bone.mOffsetMatrix);
				bool Matches = true;
				for (uint32 Column = 0; Column < 4 && Matches; ++Column)
					for (uint32 Row = 0; Row < 4; ++Row)
						if (std::abs(ExistingMatrix[Column][Row] - CandidateMatrix[Column][Row]) > 0.0001f)
						{
							Matches = false;
							break;
						}
				if (!Matches)
					throw AssetContentValidationException(AssetType::Model, ModelPath,
														  "Bone '" + Name + "' has conflicting inverse-bind matrices across meshes");
			}
		}
	}
	if (InverseBinds.empty())
		return {};

	std::unordered_map<string, const aiNode *> Nodes;
	try
	{
		IndexNodes(*Scene.mRootNode, Nodes);
	}
	catch (const std::exception &Exception)
	{
		throw AssetContentValidationException(AssetType::Model, ModelPath, Exception.what());
	}
	std::unordered_set<string> RequiredNodes;
	for (const auto &[boneName, inverseBind] : InverseBinds)
	{
		(void)inverseBind;
		const auto Found = Nodes.find(boneName);
		if (Found == Nodes.end())
		{
			throw AssetContentValidationException(AssetType::Model, ModelPath, "Bone has no matching hierarchy node: " + boneName);
		}
		for (const aiNode *Current = Found->second; Current != nullptr; Current = Current->mParent)
		{
			RequiredNodes.insert(Current->mName.C_Str());
		}
	}

	ImportedSkeleton Result;
	Result.Path = SubassetPath(ModelPath, "skeleton", 0);
	AppendSkeletonJoints(*Scene.mRootNode, InvalidJointIndex, RequiredNodes, InverseBinds, ModelPath, Result);
	if (Result.Joints.empty())
		throw AssetContentValidationException(AssetType::Model, ModelPath, "Skeletal model produced no joints");
	uint64 Signature = StableHash(ModelPath.generic_string() + ":skeleton");
	for (const SkeletonJoint &Joint : Result.Joints)
	{
		Signature ^= Joint.ID + 0x9e3779b97f4a7c15ULL + (Signature << 6U) + (Signature >> 2U);
		Signature ^= static_cast<uint64>(Joint.ParentIndex);
	}
	Result.Handle = Context.Reserve<SkeletonAsset>(AssetType::Skeleton, Result.Path);
	Context.Stage(AssetType::Skeleton, Result.Path,
				  AssetPtr<SkeletonAsset>::Make(ModelPath.stem().string() + "_Skeleton", Signature, Result.Joints));
	RootDependencies.push_back({AssetType::Skeleton, Result.Path});
	return Result;
}

[[nodiscard]] ImportedMesh ImportMesh(const aiMesh &Mesh, uint32 MeshIndex, const std::filesystem::path &ModelPath,
									  const AssetHandle<MaterialAsset> &Material, const ImportedSkeleton &Skeleton,
									  AssetImportContext &Context)
{
	if (!Mesh.HasPositions() || Mesh.mNumVertices == 0 || Mesh.mFaces == nullptr)
	{
		throw AssetContentValidationException(AssetType::Model, ModelPath, "Model mesh contains no positions or faces");
	}
	if (Mesh.HasBones() && !Skeleton.Handle)
		throw AssetContentValidationException(AssetType::Model, ModelPath, "Skeletal mesh has no validated skeleton binding");

	std::vector<glm::vec3> Positions(Mesh.mNumVertices);
	std::vector<glm::vec3> Normals(Mesh.mNumVertices);
	std::vector<glm::vec4> Tangents(Mesh.mNumVertices);
	std::array<std::vector<glm::vec2>, 6> TextureCoordinates;
	std::array<std::vector<glm::vec4>, 3> Colors;
	for (uint32 Channel = 0; Channel < TextureCoordinates.size(); ++Channel)
	{
		if (Mesh.HasTextureCoords(Channel))
			TextureCoordinates[Channel].resize(Mesh.mNumVertices);
	}
	for (uint32 Channel = 0; Channel < Colors.size(); ++Channel)
	{
		if (Mesh.HasVertexColors(Channel))
			Colors[Channel].resize(Mesh.mNumVertices);
	}
	for (uint32 Vertex = 0; Vertex < Mesh.mNumVertices; ++Vertex)
	{
		Positions[Vertex] = {Mesh.mVertices[Vertex].x, Mesh.mVertices[Vertex].y, Mesh.mVertices[Vertex].z};
		Normals[Vertex] = Mesh.HasNormals() ? glm::vec3(Mesh.mNormals[Vertex].x, Mesh.mNormals[Vertex].y, Mesh.mNormals[Vertex].z)
											: glm::vec3(0.0f, 1.0f, 0.0f);
		for (uint32 Channel = 0; Channel < TextureCoordinates.size(); ++Channel)
		{
			if (!TextureCoordinates[Channel].empty())
				TextureCoordinates[Channel][Vertex] = {Mesh.mTextureCoords[Channel][Vertex].x, Mesh.mTextureCoords[Channel][Vertex].y};
		}
		for (uint32 Channel = 0; Channel < Colors.size(); ++Channel)
		{
			if (!Colors[Channel].empty())
			{
				const aiColor4D &Color = Mesh.mColors[Channel][Vertex];
				Colors[Channel][Vertex] = {Color.r, Color.g, Color.b, Color.a};
			}
		}
		const glm::vec3 Tangent = Mesh.HasTangentsAndBitangents()
									  ? glm::vec3(Mesh.mTangents[Vertex].x, Mesh.mTangents[Vertex].y, Mesh.mTangents[Vertex].z)
									  : glm::vec3(1.0f, 0.0f, 0.0f);
		const float32 Handedness =
			Mesh.HasTangentsAndBitangents() &&
					glm::dot(glm::cross(Normals[Vertex], Tangent),
							 glm::vec3(Mesh.mBitangents[Vertex].x, Mesh.mBitangents[Vertex].y, Mesh.mBitangents[Vertex].z)) < 0.0f
				? -1.0f
				: 1.0f;
		Tangents[Vertex] = glm::vec4(Tangent, Handedness);
	}

	std::vector<glm::u16vec4> JointIndices;
	std::vector<glm::vec4> JointWeights;
	std::vector<uint32> BonePalette;
	if (Mesh.HasBones())
	{
		using Influence = std::pair<float32, uint32>;
		std::vector<std::vector<Influence>> Influences(Mesh.mNumVertices);
		for (uint32 BoneIndex = 0; BoneIndex < Mesh.mNumBones; ++BoneIndex)
		{
			const aiBone &Bone = *Mesh.mBones[BoneIndex];
			const auto Joint = Skeleton.JointIndices.find(Bone.mName.C_Str());
			if (Joint == Skeleton.JointIndices.end() || Joint->second > std::numeric_limits<uint16>::max())
			{
				throw AssetContentValidationException(AssetType::Model, ModelPath,
													  "Mesh bone cannot be represented by the skeleton joint stream");
			}
			for (uint32 WeightIndex = 0; WeightIndex < Bone.mNumWeights; ++WeightIndex)
			{
				const aiVertexWeight &Weight = Bone.mWeights[WeightIndex];
				if (Weight.mVertexId >= Mesh.mNumVertices || !std::isfinite(Weight.mWeight) || Weight.mWeight < 0.0f)
				{
					throw AssetContentValidationException(AssetType::Model, ModelPath, "Bone weight references invalid vertex data");
				}
				if (Weight.mWeight > 0.0f)
					Influences[Weight.mVertexId].emplace_back(Weight.mWeight, Joint->second);
			}
		}
		JointIndices.resize(Mesh.mNumVertices);
		JointWeights.resize(Mesh.mNumVertices);
		for (uint32 Vertex = 0; Vertex < Mesh.mNumVertices; ++Vertex)
		{
			auto &VertexInfluences = Influences[Vertex];
			std::sort(VertexInfluences.begin(), VertexInfluences.end(),
					  [](const Influence &Left, const Influence &Right) { return Left.first > Right.first; });
			if (VertexInfluences.size() > 4U)
				VertexInfluences.resize(4U);
			float32 Sum = 0.0f;
			for (const Influence &Influence : VertexInfluences)
				Sum += Influence.first;
			if (Sum <= std::numeric_limits<float32>::epsilon())
			{
				throw AssetContentValidationException(AssetType::Model, ModelPath, "Skeletal vertex has no positive bone influence");
			}
			for (uint32 InfluenceIndex = 0; InfluenceIndex < VertexInfluences.size(); ++InfluenceIndex)
			{
				JointIndices[Vertex][InfluenceIndex] = static_cast<uint16>(VertexInfluences[InfluenceIndex].second);
				JointWeights[Vertex][InfluenceIndex] = VertexInfluences[InfluenceIndex].first / Sum;
			}
		}
		std::unordered_set<uint32> RetainedJoints;
		for (uint32 Vertex = 0; Vertex < Mesh.mNumVertices; ++Vertex)
		{
			for (uint32 InfluenceIndex = 0; InfluenceIndex < 4U; ++InfluenceIndex)
			{
				if (JointWeights[Vertex][InfluenceIndex] > 0.0f)
					RetainedJoints.insert(JointIndices[Vertex][InfluenceIndex]);
			}
		}
		BonePalette.assign(RetainedJoints.begin(), RetainedJoints.end());
		std::sort(BonePalette.begin(), BonePalette.end());
	}

	std::vector<uint32> Indices;
	Indices.reserve(static_cast<usize>(Mesh.mNumFaces) * 3U);
	for (uint32 FaceIndex = 0; FaceIndex < Mesh.mNumFaces; ++FaceIndex)
	{
		const aiFace &Face = Mesh.mFaces[FaceIndex];
		if (Face.mNumIndices != 3 || Face.mIndices == nullptr)
		{
			throw AssetContentValidationException(AssetType::Model, ModelPath, "Triangulated model contains a non-triangle face");
		}
		for (uint32 Corner = 0; Corner < 3; ++Corner)
		{
			if (Face.mIndices[Corner] >= Mesh.mNumVertices)
			{
				throw AssetContentValidationException(AssetType::Model, ModelPath, "Model index references a vertex outside the mesh");
			}
			Indices.push_back(Face.mIndices[Corner]);
		}
	}
	if (Indices.empty())
		throw AssetContentValidationException(AssetType::Model, ModelPath, "Model mesh contains no renderable triangles");

	const Bounds BaseBounds = CalculateBounds(Positions);
	const string MeshName = Mesh.mName.length == 0 ? "Mesh_" + std::to_string(MeshIndex) : Mesh.mName.C_Str();
	const MaterialSlotID MaterialSlotID = StableHash(ModelPath.generic_string() + ":material:" + std::to_string(Mesh.mMaterialIndex));
	MeshAssetData Data;
	Data.Name = MeshName;
	Data.Bounds = BaseBounds;
	Data.MaterialSlots.push_back({MaterialSlotID, "Material_" + std::to_string(Mesh.mMaterialIndex), Material});
	MeshLOD LOD;
	LOD.Level = 0;
	LOD.ScreenCoverage = 1.0f;
	LOD.Bounds = BaseBounds;
	LOD.VertexStreams.push_back(
		{MeshVertexSemantic::Position, MeshVertexFormat::Float32x3, 0, sizeof(glm::vec3), Mesh.mNumVertices, ToBytes(Positions)});
	LOD.VertexStreams.push_back(
		{MeshVertexSemantic::Normal, MeshVertexFormat::Float32x3, 0, sizeof(glm::vec3), Mesh.mNumVertices, ToBytes(Normals)});
	LOD.VertexStreams.push_back(
		{MeshVertexSemantic::Tangent, MeshVertexFormat::Float32x4, 0, sizeof(glm::vec4), Mesh.mNumVertices, ToBytes(Tangents)});
	for (uint32 Channel = 0; Channel < MaterialTextureCoordinateChannelCount; ++Channel)
	{
		std::vector<glm::vec2> Coordinates = TextureCoordinates[Channel];
		if (Coordinates.empty())
			Coordinates.resize(Mesh.mNumVertices, glm::vec2(0.0f));
		LOD.VertexStreams.push_back({MeshVertexSemantic::TextureCoordinate, MeshVertexFormat::Float32x2, Channel, sizeof(glm::vec2),
									 Mesh.mNumVertices, ToBytes(Coordinates)});
	}
	for (uint32 Channel = 0; Channel < Colors.size(); ++Channel)
	{
		if (!Colors[Channel].empty())
			LOD.VertexStreams.push_back({MeshVertexSemantic::Color, MeshVertexFormat::Float32x4, Channel, sizeof(glm::vec4),
										 Mesh.mNumVertices, ToBytes(Colors[Channel])});
	}
	if (Mesh.HasBones())
	{
		LOD.VertexStreams.push_back({MeshVertexSemantic::JointIndices, MeshVertexFormat::UInt16x4, 0, sizeof(glm::u16vec4),
									 Mesh.mNumVertices, ToBytes(JointIndices)});
		LOD.VertexStreams.push_back({MeshVertexSemantic::JointWeights, MeshVertexFormat::Float32x4, 0, sizeof(glm::vec4), Mesh.mNumVertices,
									 ToBytes(JointWeights)});
	}
	if (Mesh.mNumVertices <= std::numeric_limits<uint16>::max())
	{
		std::vector<uint16> CompactIndices;
		CompactIndices.reserve(Indices.size());
		for (uint32 Index : Indices)
			CompactIndices.push_back(static_cast<uint16>(Index));
		LOD.IndexStream = {MeshIndexFormat::UInt16, static_cast<uint32>(CompactIndices.size()), ToBytes(CompactIndices)};
	}
	else
	{
		LOD.IndexStream = {MeshIndexFormat::UInt32, static_cast<uint32>(Indices.size()), ToBytes(Indices)};
	}
	LOD.Sections.push_back({.ID = StableHash(ModelPath.generic_string() + ":mesh:" + std::to_string(MeshIndex) + ":section:0"),
							.MaterialSlot = MaterialSlotID,
							.FirstIndex = 0,
							.IndexCount = static_cast<uint32>(Indices.size()),
							.BaseVertex = 0,
							.LocalBounds = BaseBounds});
	std::vector<glm::vec3> DeformedMinimum = Positions;
	std::vector<glm::vec3> DeformedMaximum = Positions;
	for (uint32 MorphIndex = 0; MorphIndex < Mesh.mNumAnimMeshes; ++MorphIndex)
	{
		if (Mesh.mAnimMeshes == nullptr || Mesh.mAnimMeshes[MorphIndex] == nullptr)
		{
			throw AssetContentValidationException(AssetType::Model, ModelPath, "Mesh contains a null morph target");
		}
		const aiAnimMesh &Morph = *Mesh.mAnimMeshes[MorphIndex];
		const string MorphName = Morph.mName.length == 0 ? "Morph_" + std::to_string(MorphIndex) : Morph.mName.C_Str();
		const MorphTargetID MorphID = StableHash(ModelPath.generic_string() + ":mesh:" + std::to_string(MeshIndex) + ":morph:" + MorphName);
		std::vector<glm::vec3> PositionDeltas(Mesh.mNumVertices, glm::vec3(0.0f));
		std::vector<glm::vec3> NormalDeltas(Mesh.mNumVertices, glm::vec3(0.0f));
		for (uint32 Vertex = 0; Vertex < Mesh.mNumVertices; ++Vertex)
		{
			if (Morph.mVertices != nullptr)
				PositionDeltas[Vertex] =
					glm::vec3(Morph.mVertices[Vertex].x, Morph.mVertices[Vertex].y, Morph.mVertices[Vertex].z) - Positions[Vertex];
			DeformedMinimum[Vertex] += glm::min(PositionDeltas[Vertex], glm::vec3(0.0f));
			DeformedMaximum[Vertex] += glm::max(PositionDeltas[Vertex], glm::vec3(0.0f));
			if (Morph.mNormals != nullptr)
				NormalDeltas[Vertex] =
					glm::vec3(Morph.mNormals[Vertex].x, Morph.mNormals[Vertex].y, Morph.mNormals[Vertex].z) - Normals[Vertex];
		}
		Data.MorphTargets.push_back({MorphID, MorphName});
		LOD.MorphTargets.push_back({.Target = MorphID,
									.PositionDeltas = {MeshVertexSemantic::MorphDelta, MeshVertexFormat::Float32x3, MorphIndex,
													   sizeof(glm::vec3), Mesh.mNumVertices, ToBytes(PositionDeltas)},
									.NormalDeltas = {MeshVertexSemantic::MorphDelta, MeshVertexFormat::Float32x3, MorphIndex,
													 sizeof(glm::vec3), Mesh.mNumVertices, ToBytes(NormalDeltas)}});
	}
	glm::vec3 DeformationMinimum = DeformedMinimum.front();
	glm::vec3 DeformationMaximum = DeformedMaximum.front();
	for (uint32 Vertex = 0; Vertex < Mesh.mNumVertices; ++Vertex)
	{
		DeformationMinimum = glm::min(DeformationMinimum, DeformedMinimum[Vertex]);
		DeformationMaximum = glm::max(DeformationMaximum, DeformedMaximum[Vertex]);
	}
	const Bounds DeformationBounds = CalculateBoundsFromExtents(DeformationMinimum, DeformationMaximum);
	Data.Bounds = DeformationBounds;
	LOD.Bounds = DeformationBounds;
	LOD.Sections.front().LocalBounds = DeformationBounds;
	Data.LODs.push_back(std::move(LOD));
	Data.DerivedDataKey = BuildMeshDerivedDataKey(ModelPath, MeshIndex, Data);

	const std::filesystem::path MeshPath = SubassetPath(ModelPath, "mesh", MeshIndex);
	std::vector<AssetDependency> MeshDependencies{{AssetType::Material, SubassetPath(ModelPath, "material", Mesh.mMaterialIndex)}};
	if (Mesh.HasBones())
	{
		std::vector<SkinningPartition::BoneInfluenceBounds> BoneBounds;
		BoneBounds.reserve(BonePalette.size());
		for (uint32 Joint : BonePalette)
		{
			bool Initialized = false;
			glm::vec3 Minimum{0.0f};
			glm::vec3 Maximum{0.0f};
			for (uint32 Vertex = 0; Vertex < Mesh.mNumVertices; ++Vertex)
			{
				bool Influenced = false;
				for (uint32 InfluenceIndex = 0; InfluenceIndex < 4U; ++InfluenceIndex)
				{
					Influenced =
						Influenced || (JointWeights[Vertex][InfluenceIndex] > 0.0f && JointIndices[Vertex][InfluenceIndex] == Joint);
				}
				if (!Influenced)
					continue;
				if (!Initialized)
				{
					Minimum = DeformedMinimum[Vertex];
					Maximum = DeformedMaximum[Vertex];
					Initialized = true;
				}
				else
				{
					Minimum = glm::min(Minimum, DeformedMinimum[Vertex]);
					Maximum = glm::max(Maximum, DeformedMaximum[Vertex]);
				}
			}
			if (!Initialized)
				throw AssetContentValidationException(AssetType::Model, ModelPath,
													  "Skinning palette contains a joint with no retained influences");
			BoneBounds.push_back({Joint, CalculateBoundsFromExtents(Minimum, Maximum)});
		}
		MeshDependencies.push_back({AssetType::Skeleton, Skeleton.Path});
		std::vector<SkinningPartition> Partitions{{.LOD = 0,
												   .Section = Data.LODs.front().Sections.front().ID,
												   .FirstIndex = 0,
												   .IndexCount = static_cast<uint32>(Indices.size()),
												   .BonePalette = std::move(BonePalette),
												   .BoneBounds = std::move(BoneBounds)}};
		auto ConcreteHandle = Context.Reserve<SkeletalMeshAsset>(AssetType::SkeletalMesh, MeshPath);
		Context.Stage(AssetType::SkeletalMesh, MeshPath,
					  AssetPtr<SkeletalMeshAsset>::Make(std::move(Data), Skeleton.Handle, std::move(Partitions)),
					  std::move(MeshDependencies));
		return {AssetHandle<MeshAsset>(std::move(ConcreteHandle)), AssetType::SkeletalMesh, DeformationBounds, MeshPath};
	}
	auto ConcreteHandle = Context.Reserve<StaticMeshAsset>(AssetType::StaticMesh, MeshPath);
	Context.Stage(AssetType::StaticMesh, MeshPath, AssetPtr<StaticMeshAsset>::Make(std::move(Data)), std::move(MeshDependencies));
	return {AssetHandle<MeshAsset>(std::move(ConcreteHandle)), AssetType::StaticMesh, DeformationBounds, MeshPath};
}

[[nodiscard]] aiVector3D SampleVectorKeys(const aiVectorKey *Keys, uint32 Count, float64 Time, const aiVector3D &Fallback)
{
	if (Keys == nullptr || Count == 0)
		return Fallback;
	if (Count == 1 || Time <= Keys[0].mTime)
		return Keys[0].mValue;
	if (Time >= Keys[Count - 1U].mTime)
		return Keys[Count - 1U].mValue;
	uint32 Upper = 1;
	while (Upper < Count && Keys[Upper].mTime < Time)
		++Upper;
	const uint32 Lower = Upper - 1U;
	const float64 Interval = Keys[Upper].mTime - Keys[Lower].mTime;
	const float32 Alpha = Interval <= 0.0 ? 0.0f : static_cast<float32>((Time - Keys[Lower].mTime) / Interval);
	return Keys[Lower].mValue + (Keys[Upper].mValue - Keys[Lower].mValue) * Alpha;
}

[[nodiscard]] aiQuaternion SampleQuaternionKeys(const aiQuatKey *Keys, uint32 Count, float64 Time, const aiQuaternion &Fallback)
{
	if (Keys == nullptr || Count == 0)
		return Fallback;
	if (Count == 1 || Time <= Keys[0].mTime)
		return Keys[0].mValue;
	if (Time >= Keys[Count - 1U].mTime)
		return Keys[Count - 1U].mValue;
	uint32 Upper = 1;
	while (Upper < Count && Keys[Upper].mTime < Time)
		++Upper;
	const uint32 Lower = Upper - 1U;
	const float64 Interval = Keys[Upper].mTime - Keys[Lower].mTime;
	const float32 Alpha = Interval <= 0.0 ? 0.0f : static_cast<float32>((Time - Keys[Lower].mTime) / Interval);
	aiQuaternion Result;
	aiQuaternion::Interpolate(Result, Keys[Lower].mValue, Keys[Upper].mValue, Alpha);
	return Result.Normalize();
}

[[nodiscard]] std::vector<AssetHandle<AnimationClipAsset>> ImportAnimations(const aiScene &Scene, const ImportedSkeleton &Skeleton,
																			const std::filesystem::path &ModelPath,
																			AssetImportContext &Context,
																			std::vector<AssetDependency> &RootDependencies)
{
	std::vector<AssetHandle<AnimationClipAsset>> Clips;
	if (Scene.mNumAnimations == 0)
		return Clips;
	std::unordered_map<string, const aiNode *> HierarchyNodes;
	IndexNodes(*Scene.mRootNode, HierarchyNodes);
	Clips.reserve(Scene.mNumAnimations);
	for (uint32 AnimationIndex = 0; AnimationIndex < Scene.mNumAnimations; ++AnimationIndex)
	{
		if (Scene.mAnimations == nullptr || Scene.mAnimations[AnimationIndex] == nullptr)
		{
			throw AssetContentValidationException(AssetType::Model, ModelPath, "Model contains a null animation");
		}
		const aiAnimation &Animation = *Scene.mAnimations[AnimationIndex];
		const float64 TicksPerSecond = Animation.mTicksPerSecond > 0.0 ? Animation.mTicksPerSecond : 25.0;
		const float64 DurationTicks = Animation.mDuration > 0.0 ? Animation.mDuration : 1.0;
		const float32 DurationSeconds = static_cast<float32>(DurationTicks / TicksPerSecond);
		std::vector<AnimationJointTrack> JointTracks;
		bool ContainsRootMotion = false;
		for (uint32 ChannelIndex = 0; ChannelIndex < Animation.mNumChannels; ++ChannelIndex)
		{
			if (Animation.mChannels == nullptr || Animation.mChannels[ChannelIndex] == nullptr)
			{
				throw AssetContentValidationException(AssetType::Model, ModelPath, "Animation contains a null joint channel");
			}
			const aiNodeAnim &Channel = *Animation.mChannels[ChannelIndex];
			const string JointName = Channel.mNodeName.C_Str();
			if (!Skeleton.Handle)
				continue;
			const auto JointIndex = Skeleton.JointIndices.find(JointName);
			if (JointIndex == Skeleton.JointIndices.end())
				continue;
			const auto HierarchyNode = HierarchyNodes.find(JointName);
			if (HierarchyNode == HierarchyNodes.end())
				throw AssetContentValidationException(AssetType::Model, ModelPath, "Animation joint is missing from the hierarchy");
			aiVector3D DefaultScale;
			aiVector3D DefaultTranslation;
			aiQuaternion DefaultRotation;
			HierarchyNode->second->mTransformation.Decompose(DefaultScale, DefaultRotation, DefaultTranslation);
			std::vector<float64> SampleTimes{0.0, DurationTicks};
			for (uint32 Key = 0; Key < Channel.mNumPositionKeys; ++Key)
				SampleTimes.push_back(Channel.mPositionKeys[Key].mTime);
			for (uint32 Key = 0; Key < Channel.mNumRotationKeys; ++Key)
				SampleTimes.push_back(Channel.mRotationKeys[Key].mTime);
			for (uint32 Key = 0; Key < Channel.mNumScalingKeys; ++Key)
				SampleTimes.push_back(Channel.mScalingKeys[Key].mTime);
			std::sort(SampleTimes.begin(), SampleTimes.end());
			SampleTimes.erase(std::unique(SampleTimes.begin(), SampleTimes.end()), SampleTimes.end());
			AnimationJointTrack Track;
			Track.ID = StableHash(ModelPath.generic_string() + ":animation:" + std::to_string(AnimationIndex) + ":joint:" + JointName);
			Track.Joint = Skeleton.Joints[JointIndex->second].ID;
			Track.Keys.reserve(SampleTimes.size());
			for (float64 SampleTime : SampleTimes)
			{
				const aiVector3D Translation =
					SampleVectorKeys(Channel.mPositionKeys, Channel.mNumPositionKeys, SampleTime, DefaultTranslation);
				const aiQuaternion Rotation =
					SampleQuaternionKeys(Channel.mRotationKeys, Channel.mNumRotationKeys, SampleTime, DefaultRotation);
				const aiVector3D Scale = SampleVectorKeys(Channel.mScalingKeys, Channel.mNumScalingKeys, SampleTime, DefaultScale);
				Track.Keys.push_back({.Time = glm::clamp(static_cast<float32>(SampleTime / TicksPerSecond), 0.0f, DurationSeconds),
									  .Translation = {Translation.x, Translation.y, Translation.z},
									  .Rotation = {Rotation.w, Rotation.x, Rotation.y, Rotation.z},
									  .Scale = {Scale.x, Scale.y, Scale.z}});
			}
			ContainsRootMotion = ContainsRootMotion || Skeleton.Joints[JointIndex->second].ParentIndex == InvalidJointIndex;
			JointTracks.push_back(std::move(Track));
		}

		std::vector<AnimationMorphTrack> MorphTracks;
		for (uint32 MorphChannelIndex = 0; MorphChannelIndex < Animation.mNumMorphMeshChannels; ++MorphChannelIndex)
		{
			if (Animation.mMorphMeshChannels == nullptr || Animation.mMorphMeshChannels[MorphChannelIndex] == nullptr)
			{
				throw AssetContentValidationException(AssetType::Model, ModelPath, "Animation contains a null morph channel");
			}
			const aiMeshMorphAnim &Channel = *Animation.mMorphMeshChannels[MorphChannelIndex];
			uint32 MeshIndex = Scene.mNumMeshes;
			for (uint32 Candidate = 0; Candidate < Scene.mNumMeshes; ++Candidate)
			{
				if (string(Scene.mMeshes[Candidate]->mName.C_Str()) == Channel.mName.C_Str())
				{
					MeshIndex = Candidate;
					break;
				}
			}
			if (MeshIndex == Scene.mNumMeshes && Scene.mNumMeshes == 1)
				MeshIndex = 0;
			if (MeshIndex == Scene.mNumMeshes)
				continue;
			const aiMesh &Mesh = *Scene.mMeshes[MeshIndex];
			for (uint32 TargetIndex = 0; TargetIndex < Mesh.mNumAnimMeshes; ++TargetIndex)
			{
				const string TargetName = Mesh.mAnimMeshes[TargetIndex]->mName.length == 0 ? "Morph_" + std::to_string(TargetIndex)
																						   : Mesh.mAnimMeshes[TargetIndex]->mName.C_Str();
				AnimationMorphTrack Track;
				Track.MorphTarget = StableHash(ModelPath.generic_string() + ":mesh:" + std::to_string(MeshIndex) + ":morph:" + TargetName);
				for (uint32 KeyIndex = 0; KeyIndex < Channel.mNumKeys; ++KeyIndex)
				{
					const aiMeshMorphKey &Key = Channel.mKeys[KeyIndex];
					float32 Weight = 0.0f;
					for (uint32 ValueIndex = 0; ValueIndex < Key.mNumValuesAndWeights; ++ValueIndex)
					{
						if (Key.mValues[ValueIndex] == TargetIndex)
							Weight = static_cast<float32>(Key.mWeights[ValueIndex]);
					}
					Track.Keys.push_back({glm::clamp(static_cast<float32>(Key.mTime / TicksPerSecond), 0.0f, DurationSeconds), Weight});
				}
				if (!Track.Keys.empty())
					MorphTracks.push_back(std::move(Track));
			}
		}

		if (JointTracks.empty() && MorphTracks.empty())
		{
			throw AssetContentValidationException(AssetType::Model, ModelPath,
												  "Animation contains no channels compatible with the imported skeleton or morph targets");
		}
		const string AnimationName = Animation.mName.length == 0 ? "Animation_" + std::to_string(AnimationIndex) : Animation.mName.C_Str();
		const std::filesystem::path AnimationPath = SubassetPath(ModelPath, "animation", AnimationIndex);
		auto Handle = Context.Reserve<AnimationClipAsset>(AssetType::AnimationClip, AnimationPath);
		std::vector<AssetDependency> Dependencies;
		if (Skeleton.Handle)
			Dependencies.push_back({AssetType::Skeleton, Skeleton.Path});
		Context.Stage(AssetType::AnimationClip, AnimationPath,
					  AssetPtr<AnimationClipAsset>::Make(AnimationName, Skeleton.Handle, DurationSeconds,
														 static_cast<float32>(TicksPerSecond), std::move(JointTracks),
														 std::move(MorphTracks), std::vector<AnimationEvent>{}, ContainsRootMotion),
					  std::move(Dependencies));
		RootDependencies.push_back({AssetType::AnimationClip, AnimationPath});
		Clips.push_back(std::move(Handle));
	}
	return Clips;
}

void AppendNodes(const aiNode &Source, uint32 ParentIndex, const glm::mat4 &ParentTransform, const std::filesystem::path &ModelPath,
				 std::span<const ImportedMesh> Meshes, std::vector<ModelNode> &Nodes, std::vector<ModelMeshInstance> &Instances,
				 Bounds &ModelBounds, bool &BoundsInitialized)
{
	const uint32 NodeIndex = static_cast<uint32>(Nodes.size());
	const string NodeName = Source.mName.length == 0 ? "Node_" + std::to_string(NodeIndex) : Source.mName.C_Str();
	const glm::mat4 LocalTransform = ToMatrix(Source.mTransformation);
	const glm::mat4 WorldTransform = ParentTransform * LocalTransform;
	Nodes.push_back({StableHash(ModelPath.generic_string() + ":node:" + std::to_string(NodeIndex) + ":" + NodeName), NodeName, ParentIndex,
					 LocalTransform});

	for (uint32 NodeMesh = 0; NodeMesh < Source.mNumMeshes; ++NodeMesh)
	{
		const uint32 MeshIndex = Source.mMeshes[NodeMesh];
		if (MeshIndex >= Meshes.size())
			throw AssetContentValidationException(AssetType::Model, ModelPath, "Model node references an invalid mesh");
		Instances.push_back({StableHash(ModelPath.generic_string() + ":node:" + std::to_string(NodeIndex) +
										":mesh:" + std::to_string(MeshIndex) + ":" + std::to_string(NodeMesh)),
							 NodeIndex, Meshes[MeshIndex].Handle});
		IncludeTransformedBounds(ModelBounds, BoundsInitialized, Meshes[MeshIndex].Bounds, WorldTransform);
	}

	for (uint32 Child = 0; Child < Source.mNumChildren; ++Child)
	{
		if (Source.mChildren[Child] == nullptr)
			throw AssetContentValidationException(AssetType::Model, ModelPath, "Model node contains a null child");
		AppendNodes(*Source.mChildren[Child], NodeIndex, WorldTransform, ModelPath, Meshes, Nodes, Instances, ModelBounds,
					BoundsInitialized);
	}
}
} // namespace

bool ModelAssetImporter::CanImport(const std::filesystem::path &Path) const
{
	const string Extension = GetNormalizedExtension(Path);
	return Extension == ".gltf" || Extension == ".glb" || Extension == ".fbx" || Extension == ".obj";
}

AssetType ModelAssetImporter::GetAssetType() const noexcept
{
	return AssetType::Model;
}

AssetImportResult ModelAssetImporter::ImportCPU(const std::filesystem::path &Path, AssetImportContext &Context) const
{
	this->ValidateImportRequest(Path);
	Assimp::Importer Importer;
	const aiScene *Scene = Importer.ReadFile(Path.string(), aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
																aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices |
																aiProcess_ImproveCacheLocality | aiProcess_ValidateDataStructure);
	if (Scene == nullptr || (Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 || Scene->mRootNode == nullptr)
	{
		throw AssetModelParseException(AssetType::Model, Path, Importer.GetErrorString());
	}
	if (Scene->mNumMeshes == 0 || Scene->mMeshes == nullptr)
	{
		throw AssetContentValidationException(AssetType::Model, Path, "Model contains no renderable meshes");
	}

	std::vector<AssetDependency> RootDependencies;
	std::unordered_map<uint32, AssetHandle<Texture2DAsset>> EmbeddedTextures;
	std::vector<AssetHandle<MaterialAsset>> Materials;
	Materials.reserve(std::max(1U, Scene->mNumMaterials));
	if (Scene->mNumMaterials == 0)
	{
		const std::filesystem::path MaterialPath = SubassetPath(Path, "material", 0);
		auto Material = Context.Reserve<MaterialAsset>(AssetType::Material, MaterialPath);
		Context.Stage(AssetType::Material, MaterialPath,
					  AssetPtr<MaterialAsset>::Make("DefaultMaterial", MaterialPipelineContract{}, PBRMaterialFactors{},
													std::vector<MaterialTextureBinding>{}));
		RootDependencies.push_back({AssetType::Material, MaterialPath});
		Materials.push_back(std::move(Material));
	}
	else
	{
		if (Scene->mMaterials == nullptr)
			throw AssetContentValidationException(AssetType::Model, Path, "Model material array is null");
		for (uint32 MaterialIndex = 0; MaterialIndex < Scene->mNumMaterials; ++MaterialIndex)
		{
			if (Scene->mMaterials[MaterialIndex] == nullptr)
				throw AssetContentValidationException(AssetType::Model, Path, "Model contains a null material");
			Materials.push_back(ImportMaterial(*Scene->mMaterials[MaterialIndex], MaterialIndex, *Scene, Path, Context, EmbeddedTextures,
											   RootDependencies));
		}
	}

	const ImportedSkeleton Skeleton = ImportSkeleton(*Scene, Path, Context, RootDependencies);

	std::vector<ImportedMesh> Meshes;
	Meshes.reserve(Scene->mNumMeshes);
	for (uint32 MeshIndex = 0; MeshIndex < Scene->mNumMeshes; ++MeshIndex)
	{
		const aiMesh *Mesh = Scene->mMeshes[MeshIndex];
		if (Mesh == nullptr || Mesh->mMaterialIndex >= Materials.size())
		{
			throw AssetContentValidationException(AssetType::Model, Path, "Model mesh or material reference is invalid");
		}
		Meshes.push_back(ImportMesh(*Mesh, MeshIndex, Path, Materials[Mesh->mMaterialIndex], Skeleton, Context));
		RootDependencies.push_back({Meshes.back().Type, Meshes.back().Path});
	}

	std::vector<ModelNode> Nodes;
	std::vector<ModelMeshInstance> MeshInstances;
	Bounds Bounds;
	bool BoundsInitialized = false;
	AppendNodes(*Scene->mRootNode, InvalidModelNodeIndex, glm::mat4(1.0f), Path, Meshes, Nodes, MeshInstances, Bounds, BoundsInitialized);
	if (MeshInstances.empty() || !BoundsInitialized)
	{
		throw AssetContentValidationException(AssetType::Model, Path, "Model hierarchy contains no renderable mesh instances");
	}

	std::vector<AssetHandle<AnimationClipAsset>> AnimationClips = ImportAnimations(*Scene, Skeleton, Path, Context, RootDependencies);
	AssetHandle<AnimationGraphAsset> DefaultAnimationGraph;
	if (!AnimationClips.empty())
	{
		const std::filesystem::path GraphPath = SubassetPath(Path, "animation-graph", 0);
		DefaultAnimationGraph = Context.Reserve<AnimationGraphAsset>(AssetType::AnimationGraph, GraphPath);
		std::vector<AnimationParameterDefinition> Parameters;
		std::vector<AnimationGraphNode> GraphNodes;
		std::vector<AnimationNodeID> ClipNodeIDs;
		std::vector<AssetDependency> GraphDependencies;
		for (uint32 ClipIndex = 0; ClipIndex < AnimationClips.size(); ++ClipIndex)
		{
			const AnimationNodeID ClipNodeID = StableHash(Path.generic_string() + ":default-graph:clip:" + std::to_string(ClipIndex));
			ClipNodeIDs.push_back(ClipNodeID);
			GraphNodes.push_back({.ID = ClipNodeID, .Type = AnimationGraphNodeType::Clip, .Clip = AnimationClips[ClipIndex]});
			GraphDependencies.push_back({AssetType::AnimationClip, SubassetPath(Path, "animation", ClipIndex)});
		}
		AnimationNodeID GraphInput = ClipNodeIDs.front();
		if (ClipNodeIDs.size() > 1U)
		{
			const AnimationParameterID StateParameter = StableHash(Path.generic_string() + ":default-graph:state");
			Parameters.push_back({StateParameter, "State", AnimationParameterType::Scalar, glm::vec4(0.0f)});
			GraphInput = StableHash(Path.generic_string() + ":default-graph:state-machine");
			GraphNodes.push_back({.ID = GraphInput,
								  .Type = AnimationGraphNodeType::StateMachine,
								  .Inputs = ClipNodeIDs,
								  .ControllingParameter = StateParameter});
		}
		const AnimationNodeID OutputNode = StableHash(Path.generic_string() + ":default-graph:output");
		GraphNodes.push_back({.ID = OutputNode, .Type = AnimationGraphNodeType::Output, .Inputs = {GraphInput}});
		Context.Stage(AssetType::AnimationGraph, GraphPath,
					  AssetPtr<AnimationGraphAsset>::Make(Path.stem().string() + "_DefaultAnimationGraph", std::move(Parameters),
														  std::move(GraphNodes), OutputNode),
					  std::move(GraphDependencies));
		RootDependencies.push_back({AssetType::AnimationGraph, GraphPath});
	}
	return AssetImportResult(AssetPtr<ModelAsset>::Make(Path.stem().string(), Bounds, std::move(Nodes), std::move(MeshInstances),
														std::move(AnimationClips), std::move(DefaultAnimationGraph)),
							 std::move(RootDependencies));
}
} // namespace resource::importer
