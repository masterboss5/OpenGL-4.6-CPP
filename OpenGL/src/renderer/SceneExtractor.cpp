#include "SceneExtractor.h"

#include "src/component/object/CObjectAnimationComponent.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/pipeline/device/Device.h"
#include "src/scene/Camera.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace renderer
{
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
									  std::span<const resource::MaterialTextureBinding> Textures, resource::AssetManager &Assets,
									  pipeline::device::Device &Device, SceneCollection &Output)
{
	const auto Binding = std::find_if(Textures.begin(), Textures.end(),
									  [Semantic](const resource::MaterialTextureBinding &Value) { return Value.Semantic == Semantic; });
	if (Binding == Textures.end())
		return 0;
	if (!Assets.RealizeGPU(Device, Binding->Texture.GetID()))
		return 0;
	auto Texture = Binding->Texture.TryPin();
	if (Texture == nullptr || Texture->GetGPUTexture() == nullptr || !Texture->GetGPUTexture()->IsResident())
		return 0;
	const bool IsColorTexture = Semantic == resource::MaterialTextureSemantic::BaseColor ||
								Semantic == resource::MaterialTextureSemantic::Emissive ||
								Semantic == resource::MaterialTextureSemantic::Specular;
	const uint64 Handle = Texture->GetGPUTexture()->GetHandle(IsColorTexture ? renderer::texture::TextureColorSpace::SRGB
																			 : renderer::texture::TextureColorSpace::Linear);
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

[[nodiscard]] std::vector<glm::mat4> ReferenceSkinPose(const resource::SkeletonAsset &Skeleton)
{
	std::vector<glm::mat4> Global(Skeleton.GetJoints().size());
	std::vector<glm::mat4> Skin(Skeleton.GetJoints().size());
	for (uint32 JointIndex = 0; JointIndex < Skeleton.GetJoints().size(); ++JointIndex)
	{
		const resource::SkeletonJoint &Joint = Skeleton.GetJoints()[JointIndex];
		Global[JointIndex] = Joint.ParentIndex == resource::InvalidJointIndex ? Joint.ReferenceLocalTransform
																			  : Global[Joint.ParentIndex] * Joint.ReferenceLocalTransform;
		Skin[JointIndex] = Global[JointIndex] * Joint.InverseBindMatrix;
	}
	return Skin;
}
} // namespace

void SceneExtractor::Extract(const world::Scene &Scene, const Camera &Camera, SceneCollection &Output) const
{
	auto Access = Scene.Read();
	for (const components::CObjectMeshComponent &Component : Access.Components<components::CObjectMeshComponent>())
	{
		if (!Component.IsEnabled() || !HasFlag(Component.GetVisibility(), components::MeshVisibilityFlags::Visible))
			continue;
		const world::ObjectHandle Owner = Component.GetOwner();
		const auto TransformHandle = Access.GetComponent<components::CObjectTransformComponent>(Owner);
		if (!TransformHandle.IsValid())
			throw std::logic_error("Mesh component owner lost its required Transform component");
		const glm::mat4 ObjectTransform = Access.Resolve(TransformHandle).GetMatrix();
		const components::CObjectAnimationComponent *AnimationComponent = nullptr;
		const auto AnimationHandle = Access.GetComponent<components::CObjectAnimationComponent>(Owner);
		if (AnimationHandle.IsValid())
			AnimationComponent = &Access.Resolve(AnimationHandle);

		auto Model = Component.GetModel().TryPin();
		if (Model == nullptr)
			continue;
		std::vector<glm::mat4> NodeTransforms(Model->GetNodes().size());
		std::unordered_map<resource::AssetID, uint32> SkinPaletteOffsets;
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
			const RenderTransformHistoryKey HistoryKey{Scene.GetID(), Owner.Slot, Owner.Generation, Instance.ID};
			const auto PreviousTransform = this->PreviousTransforms->find(HistoryKey);
			const glm::mat4 ResolvedPreviousTransform =
				PreviousTransform == this->PreviousTransforms->end() ? CurrentTransform : PreviousTransform->second;
			this->CurrentTransforms->insert_or_assign(HistoryKey, CurrentTransform);
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
				if (CachedPalette != SkinPaletteOffsets.end())
					SkinPaletteOffset = CachedPalette->second;
				else
				{
					std::span<const glm::mat4> Current;
					std::span<const glm::mat4> Previous;
					std::vector<glm::mat4> Fallback;
					if (AnimationComponent != nullptr)
					{
						const auto State = std::find_if(
							AnimationComponent->GetRigStates().begin(), AnimationComponent->GetRigStates().end(),
							[&SkeletonID](const components::AnimationRigRuntimeState &Value) { return Value.Skeleton == SkeletonID; });
						if (State != AnimationComponent->GetRigStates().end())
						{
							Current = State->CurrentPose;
							Previous = State->PreviousPose;
						}
					}
					if (Current.empty())
					{
						auto Skeleton = SkeletalMesh.GetSkeleton().Pin();
						Fallback = ReferenceSkinPose(*Skeleton);
						Current = Fallback;
						Previous = Fallback;
						Output.RetainAsset(std::move(Skeleton));
					}
					if (Current.size() != Previous.size())
						Previous = Current;
					SkinPaletteOffset = Output.AppendSkinningPalette(Current, Previous);
					SkinPaletteOffsets.emplace(SkeletonID, SkinPaletteOffset);
				}
				Skinned = true;
			}
			const uint32 LODIndex = this->SelectLOD(*Mesh, Component.GetLODPolicy(), NodeTransforms[Instance.NodeIndex], Camera);
			MeshGPUResource &GPUResource = this->MeshCache->Realize(Mesh, Output.GetFrameNumber());
			const MeshGPULOD &GPULOD = GPUResource.GetLOD(LODIndex, Output.GetFrameNumber());
			const resource::MeshLOD &SourceLOD = Mesh->GetLODs()[LODIndex];
			uint32 MorphWeightOffset = 0;
			uint32 MorphWeightCount = 0;
			if (AnimationComponent != nullptr && GPULOD.MorphDeltaBuffer != 0)
			{
				std::vector<GPUMorphWeightRecord> ActiveWeights;
				ActiveWeights.reserve(AnimationComponent->GetMorphWeights().size());
				for (const components::AnimationMorphWeight &Weight : AnimationComponent->GetMorphWeights())
				{
					if (std::abs(Weight.Weight) <= std::numeric_limits<float32>::epsilon() &&
						std::abs(Weight.PreviousWeight) <= std::numeric_limits<float32>::epsilon())
						continue;
					const auto Target = GPULOD.MorphTargetIndices.find(Weight.Target);
					if (Target != GPULOD.MorphTargetIndices.end())
						ActiveWeights.push_back({Target->second * GPULOD.MorphVertexCount, Weight.Weight, Weight.PreviousWeight});
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
				auto MaterialHandle = this->ResolveMaterial(Component, Instance.ID, *Slot);
				auto Material = MaterialHandle.TryPin();
				if (Material == nullptr)
					continue;
				const resource::MaterialPipelineContract &MaterialContract = Material->GetPipelineContract();
				const resource::MaterialBlendMode BlendMode = MaterialContract.BlendMode;
				const glm::vec4 WorldBounds =
					ResolveSectionWorldBounds(*Mesh, SourceLOD, Section, CurrentTransform, SkinPaletteOffset, Output);
				Output.Submit({.VertexArray = GPULOD.VertexArray,
							   .VertexDescriptor = GPULOD.VertexDescriptor.get(),
							   .IndexFormat = GPULOD.IndexFormat == resource::MeshIndexFormat::UInt16 ? RenderIndexFormat::UInt16
																									  : RenderIndexFormat::UInt32,
							   .FirstIndex = Section.FirstIndex,
							   .IndexCount = Section.IndexCount,
							   .BaseVertex = Section.BaseVertex,
							   .MaterialGeneration = Material->GetUUID(),
							   .Material = this->BuildMaterialRecord(Material, Output),
							   .Transform = CurrentTransform,
							   .PreviousTransform = ResolvedPreviousTransform,
							   .WorldBounds = WorldBounds,
							   .ObjectID = Owner.Slot,
							   .LayerMask = Component.GetRenderLayerMask(),
							   .Revision = Component.GetModel().GetPublishedGeneration(),
							   .SkinPaletteOffset = SkinPaletteOffset,
							   .PreviousSkinPaletteOffset = SkinPaletteOffset,
							   .MorphDeltaBuffer = GPULOD.MorphDeltaBuffer,
							   .MorphVertexCount = GPULOD.MorphVertexCount,
							   .MorphWeightOffset = MorphWeightOffset,
							   .MorphWeightCount = MorphWeightCount,
							   .Skinned = Skinned,
							   .Transparent = BlendMode == resource::MaterialBlendMode::Translucent ||
											  BlendMode == resource::MaterialBlendMode::Additive,
							   .CastsShadows = HasFlag(Component.GetVisibility(), components::MeshVisibilityFlags::CastsShadows) &&
											   MaterialContract.CastsShadows,
							   .ReceivesShadows = HasFlag(Component.GetVisibility(), components::MeshVisibilityFlags::ReceivesShadows) &&
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
	const float32 Coverage = glm::clamp(WorldSphere.w / (Distance * std::tan(glm::radians(Camera.FOV) * 0.5f)), 0.0f, 1.0f);
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

resource::AssetHandle<resource::MaterialInterfaceAsset> SceneExtractor::ResolveMaterial(const components::CObjectMeshComponent &Component,
																						resource::ModelMeshInstanceID MeshInstance,
																						const resource::MeshMaterialSlot &Slot) const
{
	const auto Overrides = Component.GetMaterialOverrides();
	const auto Found = std::find_if(Overrides.begin(), Overrides.end(), [MeshInstance, &Slot](const components::MeshMaterialOverride &Value)
									{ return Value.MeshInstance == MeshInstance && Value.MaterialSlot == Slot.ID; });
	return Found == Overrides.end() ? Slot.DefaultMaterial : Found->Material;
}

GPUMaterialRecord SceneExtractor::BuildMaterialRecord(const resource::AssetPtr<resource::MaterialInterfaceAsset> &Material,
													  SceneCollection &Output) const
{
	const resource::PBRMaterialFactors &Factors = Material->GetFactors();
	const auto Textures = Material->GetTextures();
	return {
		.BaseColorTexture = TextureHandleFor(resource::MaterialTextureSemantic::BaseColor, Textures, *this->Assets, *this->Device, Output),
		.NormalTexture = TextureHandleFor(resource::MaterialTextureSemantic::Normal, Textures, *this->Assets, *this->Device, Output),
		.MetallicRoughnessTexture =
			TextureHandleFor(resource::MaterialTextureSemantic::MetallicRoughness, Textures, *this->Assets, *this->Device, Output),
		.OcclusionTexture = TextureHandleFor(resource::MaterialTextureSemantic::Occlusion, Textures, *this->Assets, *this->Device, Output),
		.EmissiveTexture = TextureHandleFor(resource::MaterialTextureSemantic::Emissive, Textures, *this->Assets, *this->Device, Output),
		.SpecularTexture = TextureHandleFor(resource::MaterialTextureSemantic::Specular, Textures, *this->Assets, *this->Device, Output),
		.TransmissionTexture =
			TextureHandleFor(resource::MaterialTextureSemantic::Transmission, Textures, *this->Assets, *this->Device, Output),
		.TextureCoordinateSelectors = PackTextureCoordinateSelectors(Textures),
		.BaseColorFactor = Factors.BaseColor,
		.EmissiveAndMetallic = glm::vec4(Factors.Emissive, Factors.Metallic),
		.RoughnessTransmissionIOR = glm::vec4(Factors.Roughness, Factors.Transmission, Factors.IndexOfRefraction, Factors.AlphaCutoff),
		.TextureControls = glm::vec4(Factors.NormalScale, Factors.OcclusionStrength, Factors.Specular, 0.0f)};
}
} // namespace renderer
