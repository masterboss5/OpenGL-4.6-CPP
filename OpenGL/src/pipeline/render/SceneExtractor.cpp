#include "SceneExtractor.h"

#include "src/component/object/CObjectAnimationComponent.h"
#include "src/component/object/CObjectLightComponents.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/pipeline/device/Device.h"
#include "src/scene/Camera.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace pipeline::render
{
using pipeline::mesh::MeshGPULOD;
using pipeline::mesh::MeshGPUResource;

usize RenderTransformHistoryKeyHash::operator()(const RenderTransformHistoryKey &Key) const noexcept
{
	uint64 Hash = 1469598103934665603ULL;
	const auto Mix = [&Hash](const uint64 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ULL;
	};
	Mix(Key.Scene);
	Mix(Key.ObjectSlot);
	Mix(Key.ObjectGeneration);
	Mix(Key.MeshInstance);
	return static_cast<usize>(Hash);
}

namespace
{
[[nodiscard]] bool HasFlag(components::MeshVisibilityFlags Value, components::MeshVisibilityFlags Flag) noexcept
{
	return (static_cast<uint32>(Value) & static_cast<uint32>(Flag)) != 0;
}

[[nodiscard]] glm::vec4 TransformSphere(const glm::vec4 &Sphere, const glm::mat4 &Transform)
{
	const glm::vec3 Center = glm::vec3(Transform * glm::vec4(glm::vec3(Sphere), 1.0f));
	const float32 MaximumScale =
		std::max({glm::length(glm::vec3(Transform[0])), glm::length(glm::vec3(Transform[1])), glm::length(glm::vec3(Transform[2]))});
	return glm::vec4(Center, Sphere.w * MaximumScale);
}

[[nodiscard]] uint64 TextureHandleFor(resource::MaterialTextureSemantic Semantic,
									  std::span<const resource::MaterialTextureBinding> Textures, SceneCollection &Output)
{
	const auto Binding = std::find_if(Textures.begin(), Textures.end(),
									  [Semantic](const resource::MaterialTextureBinding &Value) { return Value.Semantic == Semantic; });
	if (Binding == Textures.end())
		return 0;
	auto Texture = Binding->Texture.TryPin();
	if (Texture == nullptr || Texture->GetGPUTexture() == nullptr || !Texture->GetGPUTexture()->IsResident())
		return 0;
	const bool IsColorTexture = Semantic == resource::MaterialTextureSemantic::BaseColor ||
								Semantic == resource::MaterialTextureSemantic::Emissive ||
								Semantic == resource::MaterialTextureSemantic::Specular;
	const uint64 Handle = Texture->GetGPUTexture()->GetHandle(IsColorTexture ? pipeline::texture::TextureColorSpace::SRGB
																			 : pipeline::texture::TextureColorSpace::Linear);
	Output.RetainAsset(std::move(Texture));
	return Handle;
}

[[nodiscard]] glm::vec4 EncloseSpheres(const glm::vec4 &Left, const glm::vec4 &Right)
{
	const glm::vec3 Offset = glm::vec3(Right) - glm::vec3(Left);
	const float32 Distance = glm::length(Offset);
	if (Left.w >= Distance + Right.w)
		return Left;
	if (Right.w >= Distance + Left.w)
		return Right;
	const float32 Radius = (Distance + Left.w + Right.w) * 0.5f;
	const glm::vec3 Center =
		Distance <= std::numeric_limits<float32>::epsilon() ? glm::vec3(Left) : glm::vec3(Left) + Offset * ((Radius - Left.w) / Distance);
	return glm::vec4(Center, Radius);
}

[[nodiscard]] glm::vec4 ResolveSectionWorldBounds(const resource::MeshAsset &Mesh, const resource::MeshLOD &LOD,
												  const resource::MeshSection &Section, const glm::mat4 &WorldTransform,
												  const uint32 SkinPaletteOffset, const SceneCollection &Output)
{
	if (Mesh.GetKind() != resource::MeshKind::Skeletal)
		return TransformSphere(Section.LocalBounds.Sphere, WorldTransform);
	const auto &SkeletalMesh = static_cast<const resource::SkeletalMeshAsset &>(Mesh);
	const auto Partitions = SkeletalMesh.GetSkinningPartitions();
	const auto Partition = std::find_if(Partitions.begin(), Partitions.end(), [&LOD, &Section](const resource::SkinningPartition &Value)
										{ return Value.LOD == LOD.Level && Value.Section == Section.ID; });
	if (Partition == Partitions.end() || Partition->BoneBounds.empty())
		throw std::logic_error("Validated skeletal mesh section has no deformation bounds");
	const auto &SkinMatrices = Output.GetSkinningMatrices();
	glm::vec4 LocalSphere;
	bool Initialized = false;
	for (const resource::SkinningPartition::BoneInfluenceBounds &Bone : Partition->BoneBounds)
	{
		const uint64 MatrixIndex = static_cast<uint64>(SkinPaletteOffset) + Bone.Joint;
		if (MatrixIndex >= SkinMatrices.size())
			throw std::logic_error("Skeletal deformation bounds reference a skin matrix outside the current palette");
		const glm::vec4 BoneSphere = TransformSphere(Bone.LocalBounds.Sphere, SkinMatrices[static_cast<usize>(MatrixIndex)].Current);
		LocalSphere = Initialized ? EncloseSpheres(LocalSphere, BoneSphere) : BoneSphere;
		Initialized = true;
	}
	return TransformSphere(LocalSphere, WorldTransform);
}

[[nodiscard]] uint64 PackTextureCoordinateSelectors(std::span<const resource::MaterialTextureBinding> Textures)
{
	uint64 Selectors = 0;
	for (const resource::MaterialTextureBinding &Binding : Textures)
	{
		if (Binding.TextureCoordinateChannel >= resource::MaterialTextureCoordinateChannelCount)
			throw std::logic_error("Validated material contains an unsupported texture-coordinate channel");
		const uint32 Shift = static_cast<uint32>(Binding.Semantic) * 4U;
		Selectors |= static_cast<uint64>(Binding.TextureCoordinateChannel) << Shift;
	}
	return Selectors;
}

void BuildReferenceSkinPose(const resource::SkeletonAsset &Skeleton, std::vector<glm::mat4> &Global, std::vector<glm::mat4> &Skin)
{
	Global.resize(Skeleton.GetJoints().size());
	Skin.resize(Skeleton.GetJoints().size());
	for (uint32 JointIndex = 0; JointIndex < Skeleton.GetJoints().size(); ++JointIndex)
	{
		const resource::SkeletonJoint &Joint = Skeleton.GetJoints()[JointIndex];
		Global[JointIndex] = Joint.ParentIndex == resource::InvalidJointIndex ? Joint.ReferenceLocalTransform
																			  : Global[Joint.ParentIndex] * Joint.ReferenceLocalTransform;
		Skin[JointIndex] = Global[JointIndex] * Joint.InverseBindMatrix;
	}
}
} // namespace

void SceneExtractor::Extract(const world::Scene &Scene, const Camera &Camera, SceneCollection &Output) const
{
	SceneRenderSnapshotBuilder::BuildInto(Scene, this->Scratch->SceneSnapshot, {}, this->Scratch->SceneSnapshotBuildScratch);
	this->Extract(this->Scratch->SceneSnapshot, Camera, Output);
}

void SceneExtractor::Extract(const SceneRenderSnapshot &Snapshot, const Camera &Camera, SceneCollection &Output) const
{
	for (const DirectionalLightSource &Light : Snapshot.DirectionalLights)
		Output.AddDirectionalLight(Light);
	for (const PointLightSource &Light : Snapshot.PointLights)
		Output.AddPointLight(Light);
	for (const SpotLightSource &Light : Snapshot.SpotLights)
		Output.AddSpotLight(Light);
	for (const SceneMeshSnapshot &MeshSnapshot : Snapshot.Meshes)
	{
		const world::ObjectHandle Owner = MeshSnapshot.Owner;
		const PickID ObjectPickID = Output.RegisterPickObject(Owner);
		const glm::mat4 ObjectTransform = MeshSnapshot.ObjectTransform;
		const SceneAnimationSnapshot *Animation = MeshSnapshot.Animation.has_value() ? &*MeshSnapshot.Animation : nullptr;
		resource::AssetPtr<resource::ModelAsset> Model = MeshSnapshot.Model;
		std::vector<glm::mat4> &NodeTransforms = this->Scratch->NodeTransforms;
		NodeTransforms.resize(Model->GetNodes().size());
		std::unordered_map<resource::AssetID, SceneExtractorScratch::SkinPaletteEntry> &SkinPaletteOffsets =
			this->Scratch->SkinPaletteOffsets;
		++this->Scratch->SkinPaletteGeneration;
		if (this->Scratch->SkinPaletteGeneration == 0)
		{
			SkinPaletteOffsets.clear();
			this->Scratch->SkinPaletteGeneration = 1;
		}
		const uint64 SkinPaletteGeneration = this->Scratch->SkinPaletteGeneration;
		for (uint32 NodeIndex = 0; NodeIndex < Model->GetNodes().size(); ++NodeIndex)
		{
			const resource::ModelNode &Node = Model->GetNodes()[NodeIndex];
			const glm::mat4 Parent =
				Node.ParentIndex == resource::InvalidModelNodeIndex ? ObjectTransform : NodeTransforms[Node.ParentIndex];
			NodeTransforms[NodeIndex] = Parent * Node.LocalTransform;
		}

		for (const resource::ModelMeshInstance &Instance : Model->GetMeshInstances())
		{
			const glm::mat4 &CurrentTransform = NodeTransforms[Instance.NodeIndex];
			const RenderTransformHistoryKey HistoryKey{Snapshot.SceneID, Owner.Slot, Owner.Generation, Instance.ID};
			const auto PreviousTransform = this->PreviousTransforms->find(HistoryKey);
			const glm::mat4 ResolvedPreviousTransform =
				PreviousTransform == this->PreviousTransforms->end() || PreviousTransform->second.Generation != this->PreviousGeneration
					? CurrentTransform
					: PreviousTransform->second.Transform;
			this->CurrentTransforms->insert_or_assign(
				HistoryKey, RenderTransformHistoryEntry{.Transform = CurrentTransform, .Generation = this->CurrentGeneration});
			auto Mesh = Instance.Mesh.TryPin();
			if (Mesh == nullptr)
				continue;
			bool Skinned = false;
			uint32 SkinPaletteOffset = 0;
			if (Mesh->GetKind() == resource::MeshKind::Skeletal)
			{
				const auto &SkeletalMesh = static_cast<const resource::SkeletalMeshAsset &>(*Mesh);
				const resource::AssetID SkeletonID = SkeletalMesh.GetSkeleton().GetID();
				const auto CachedPalette = SkinPaletteOffsets.find(SkeletonID);
				if (CachedPalette != SkinPaletteOffsets.end() && CachedPalette->second.Generation == SkinPaletteGeneration)
					SkinPaletteOffset = CachedPalette->second.Offset;
				else
				{
					std::span<const glm::mat4> Current;
					std::span<const glm::mat4> Previous;
					this->Scratch->FallbackGlobalPose.clear();
					this->Scratch->FallbackSkinPose.clear();
					if (Animation != nullptr)
					{
						const auto State = std::find_if(Animation->RigStates.begin(), Animation->RigStates.end(),
														[&SkeletonID](const components::AnimationRigRuntimeState &Value)
														{ return Value.Skeleton == SkeletonID; });
						if (State != Animation->RigStates.end())
						{
							Current = State->CurrentPose;
							Previous = State->PreviousPose;
						}
					}
					if (Current.empty())
					{
						auto Skeleton = SkeletalMesh.GetSkeleton().Pin();
						if (Skeleton == nullptr)
							throw std::runtime_error("Scene extraction encountered an unavailable skeletal-mesh skeleton");
						BuildReferenceSkinPose(*Skeleton, this->Scratch->FallbackGlobalPose, this->Scratch->FallbackSkinPose);
						Current = this->Scratch->FallbackSkinPose;
						Previous = this->Scratch->FallbackSkinPose;
						Output.RetainAsset(std::move(Skeleton));
					}
					if (Current.size() != Previous.size())
						Previous = Current;
					SkinPaletteOffset = Output.AppendSkinningPalette(Current, Previous);
					SkinPaletteOffsets.insert_or_assign(SkeletonID, SceneExtractorScratch::SkinPaletteEntry{
																		.Offset = SkinPaletteOffset, .Generation = SkinPaletteGeneration});
				}
				Skinned = true;
			}
			const uint32 LODIndex = this->SelectLOD(*Mesh, MeshSnapshot.LODPolicy, NodeTransforms[Instance.NodeIndex], Camera);
			const MeshGPULOD *GPULOD = this->MeshCache->TryGetLOD(Mesh, LODIndex, Output.GetFrameNumber());
			if (GPULOD == nullptr)
				continue;
			const resource::MeshLOD &SourceLOD = Mesh->GetLODs()[LODIndex];
			uint32 MorphWeightOffset = 0;
			uint32 MorphWeightCount = 0;
			if (Animation != nullptr && GPULOD->MorphDeltaBuffer != 0)
			{
				std::vector<GPUMorphWeightRecord> &ActiveWeights = this->Scratch->ActiveMorphWeights;
				ActiveWeights.clear();
				if (ActiveWeights.capacity() < Animation->MorphWeights.size())
					ActiveWeights.reserve(Animation->MorphWeights.size());
				for (const components::AnimationMorphWeight &Weight : Animation->MorphWeights)
				{
					if (std::abs(Weight.Weight) <= std::numeric_limits<float32>::epsilon() &&
						std::abs(Weight.PreviousWeight) <= std::numeric_limits<float32>::epsilon())
						continue;
					const auto Target = GPULOD->MorphTargetIndices.find(Weight.Target);
					if (Target != GPULOD->MorphTargetIndices.end())
						ActiveWeights.push_back({Target->second * GPULOD->MorphVertexCount, Weight.Weight, Weight.PreviousWeight});
				}
				if (!ActiveWeights.empty())
				{
					MorphWeightOffset = Output.AppendMorphWeights(ActiveWeights);
					MorphWeightCount = static_cast<uint32>(ActiveWeights.size());
				}
			}

			for (const resource::MeshSection &Section : SourceLOD.Sections)
			{
				const resource::MeshMaterialSlot *Slot = Mesh->FindMaterialSlot(Section.MaterialSlot);
				if (Slot == nullptr)
					throw std::logic_error("Validated Mesh section lost its material slot");
				auto MaterialHandle = this->ResolveMaterial(MeshSnapshot.MaterialOverrides, Instance.ID, *Slot);
				auto Material = MaterialHandle.TryPin();
				if (Material == nullptr)
					continue;
				const resource::MaterialPipelineContract &MaterialContract = Material->GetPipelineContract();
				const resource::MaterialBlendMode BlendMode = MaterialContract.BlendMode;
				const glm::vec4 WorldBounds =
					ResolveSectionWorldBounds(*Mesh, SourceLOD, Section, CurrentTransform, SkinPaletteOffset, Output);
				Output.Submit({.VertexArray = GPULOD->VertexArray,
							   .VertexDescriptor = GPULOD->VertexDescriptor.get(),
							   .IndexFormat = GPULOD->IndexFormat == resource::MeshIndexFormat::UInt16 ? RenderIndexFormat::UInt16
																									   : RenderIndexFormat::UInt32,
							   .FirstIndex = Section.FirstIndex,
							   .IndexCount = Section.IndexCount,
							   .BaseVertex = Section.BaseVertex,
							   .MaterialGeneration = Material->GetUUID(),
							   .Material = this->BuildMaterialRecord(Material, Output),
							   .Transform = CurrentTransform,
							   .PreviousTransform = ResolvedPreviousTransform,
							   .WorldBounds = WorldBounds,
							   .ObjectID = ObjectPickID,
							   .LayerMask = MeshSnapshot.RenderLayerMask,
							   .Revision = MeshSnapshot.ModelPublishedGeneration,
							   .SkinPaletteOffset = SkinPaletteOffset,
							   .PreviousSkinPaletteOffset = SkinPaletteOffset,
							   .MorphDeltaBuffer = GPULOD->MorphDeltaBuffer,
							   .MorphVertexCount = GPULOD->MorphVertexCount,
							   .MorphWeightOffset = MorphWeightOffset,
							   .MorphWeightCount = MorphWeightCount,
							   .Skinned = Skinned,
							   .Transparent = BlendMode == resource::MaterialBlendMode::Translucent ||
											  BlendMode == resource::MaterialBlendMode::Additive,
							   .CastsShadows = HasFlag(MeshSnapshot.Visibility, components::MeshVisibilityFlags::CastsShadows) &&
											   MaterialContract.CastsShadows,
							   .ReceivesShadows = HasFlag(MeshSnapshot.Visibility, components::MeshVisibilityFlags::ReceivesShadows) &&
												  MaterialContract.ReceivesShadows,
							   .Masked = BlendMode == resource::MaterialBlendMode::Masked,
							   .TwoSided = MaterialContract.TwoSided});
				Output.RetainAsset(std::move(Material));
			}
			Output.RetainAsset(std::move(Mesh));
		}
		Output.RetainAsset(std::move(Model));
	}
}

uint32 SceneExtractor::SelectLOD(const resource::MeshAsset &Mesh, const components::MeshLODPolicy &Policy, const glm::mat4 &WorldTransform,
								 const Camera &Camera) const
{
	const auto LODs = Mesh.GetLODs();
	if (LODs.empty())
		throw std::logic_error("Validated Mesh asset has no LODs");
	if (Policy.Mode == components::MeshLODSelectionMode::Forced)
	{
		if (Policy.ForcedLOD >= LODs.size())
			throw std::out_of_range("Forced mesh LOD is unavailable");
		return Policy.ForcedLOD;
	}
	const glm::vec4 WorldSphere = TransformSphere(Mesh.GetBounds().Sphere, WorldTransform);
	const float32 Distance = std::max(glm::distance(Camera.Position, glm::vec3(WorldSphere)) - WorldSphere.w, 0.001f);
	const float32 Coverage = Camera.Projection == CameraProjectionMode::Orthographic
								 ? glm::clamp(WorldSphere.w / (Camera.OrthographicHeight * 0.5f), 0.0f, 1.0f)
								 : glm::clamp(WorldSphere.w / (Distance * std::tan(glm::radians(Camera.FOV) * 0.5f)), 0.0f, 1.0f);
	uint32 Selected = 0;
	for (uint32 LOD = 1; LOD < LODs.size(); ++LOD)
	{
		if (Coverage < LODs[LOD - 1].ScreenCoverage)
			Selected = LOD;
	}
	if (Policy.Mode == components::MeshLODSelectionMode::Biased)
	{
		const int64 Biased = static_cast<int64>(Selected) + Policy.Bias;
		Selected = static_cast<uint32>(std::clamp<int64>(Biased, 0, static_cast<int64>(LODs.size() - 1)));
	}
	return Selected;
}

resource::AssetHandle<resource::MaterialInterfaceAsset> SceneExtractor::ResolveMaterial(
	const std::span<const components::MeshMaterialOverride> Overrides, const resource::ModelMeshInstanceID MeshInstance,
	const resource::MeshMaterialSlot &Slot) const
{
	const auto Found = std::find_if(Overrides.begin(), Overrides.end(), [MeshInstance, &Slot](const components::MeshMaterialOverride &Value)
									{ return Value.MeshInstance == MeshInstance && Value.MaterialSlot == Slot.ID; });
	return Found == Overrides.end() ? Slot.DefaultMaterial : Found->Material;
}

GPUMaterialRecord SceneExtractor::BuildMaterialRecord(const resource::AssetPtr<resource::MaterialInterfaceAsset> &Material,
													  SceneCollection &Output) const
{
	const resource::PBRMaterialFactors &Factors = Material->GetFactors();
	const auto Textures = Material->GetTextures();
	return {.BaseColorTexture = TextureHandleFor(resource::MaterialTextureSemantic::BaseColor, Textures, Output),
			.NormalTexture = TextureHandleFor(resource::MaterialTextureSemantic::Normal, Textures, Output),
			.MetallicRoughnessTexture = TextureHandleFor(resource::MaterialTextureSemantic::MetallicRoughness, Textures, Output),
			.OcclusionTexture = TextureHandleFor(resource::MaterialTextureSemantic::Occlusion, Textures, Output),
			.EmissiveTexture = TextureHandleFor(resource::MaterialTextureSemantic::Emissive, Textures, Output),
			.SpecularTexture = TextureHandleFor(resource::MaterialTextureSemantic::Specular, Textures, Output),
			.TransmissionTexture = TextureHandleFor(resource::MaterialTextureSemantic::Transmission, Textures, Output),
			.TextureCoordinateSelectors = PackTextureCoordinateSelectors(Textures),
			.BaseColorFactor = Factors.BaseColor,
			.EmissiveAndMetallic = glm::vec4(Factors.Emissive, Factors.Metallic),
			.RoughnessTransmissionIOR = glm::vec4(Factors.Roughness, Factors.Transmission, Factors.IndexOfRefraction, Factors.AlphaCutoff),
			.TextureControls = glm::vec4(Factors.NormalScale, Factors.OcclusionStrength, Factors.Specular, 0.0f)};
}
} // namespace pipeline::render
