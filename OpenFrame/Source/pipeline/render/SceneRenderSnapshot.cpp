#include "SceneRenderSnapshot.h"

#include "Source/component/object/CObjectHierarchyComponent.h"
#include "Source/component/object/CObjectCameraComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectLightComponents.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/scene/Scene.h"
#include "Source/scene/SceneTransformSnapshot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <gtc/constants.hpp>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace pipeline::render
{
namespace
{
[[nodiscard]] bool HasFlag(const components::MeshVisibilityFlags Value, const components::MeshVisibilityFlags Flag) noexcept
{
	return (static_cast<uint32>(Value) & static_cast<uint32>(Flag)) != 0;
}

[[nodiscard]] float32 QuadraticForRange(const glm::vec3 &Radiance, const float32 Range)
{
	constexpr float32 MinimumContribution = 0.01f;
	const float32 Peak = std::max({Radiance.r, Radiance.g, Radiance.b, MinimumContribution * 1.01f});
	return std::max((Peak / MinimumContribution - 1.0f) / (Range * Range), std::numeric_limits<float32>::epsilon());
}

[[nodiscard]] LightShadowParameters CaptureShadowParameters(const components::LightShadowSettings &Settings) noexcept
{
	return {.Resolution = static_cast<uint32>(Settings.Resolution),
			.ConstantBias = Settings.ConstantBias,
			.SlopeBias = Settings.SlopeBias,
			.NormalBias = Settings.NormalBias,
			.FilterRadius = Settings.FilterRadius};
}

void AddDebugLine(SceneRenderSnapshot &Snapshot, const glm::vec3 Start, const glm::vec3 End, const glm::vec4 Color,
				  const SceneDebugLineCategory Category)
{
	Snapshot.DebugLines.push_back({.Start = Start, .End = End, .Color = Color, .Category = Category});
}

[[nodiscard]] std::array<glm::vec3, 8> TransformBounds(const resource::Bounds &Bounds, const glm::mat4 &Transform)
{
	const std::array Local{
		glm::vec3(Bounds.Minimum.x, Bounds.Minimum.y, Bounds.Minimum.z), glm::vec3(Bounds.Maximum.x, Bounds.Minimum.y, Bounds.Minimum.z),
		glm::vec3(Bounds.Maximum.x, Bounds.Maximum.y, Bounds.Minimum.z), glm::vec3(Bounds.Minimum.x, Bounds.Maximum.y, Bounds.Minimum.z),
		glm::vec3(Bounds.Minimum.x, Bounds.Minimum.y, Bounds.Maximum.z), glm::vec3(Bounds.Maximum.x, Bounds.Minimum.y, Bounds.Maximum.z),
		glm::vec3(Bounds.Maximum.x, Bounds.Maximum.y, Bounds.Maximum.z), glm::vec3(Bounds.Minimum.x, Bounds.Maximum.y, Bounds.Maximum.z)};
	std::array<glm::vec3, 8> Result;
	for (uint32 Index = 0; Index < Result.size(); ++Index)
		Result[Index] = glm::vec3(Transform * glm::vec4(Local[Index], 1.0f));
	return Result;
}

void AddBoundsLines(SceneRenderSnapshot &Snapshot, const SceneDebugBounds &Bounds, const glm::vec4 Color,
					const SceneDebugLineCategory Category)
{
	static constexpr std::array<std::array<uint8, 2>, 12> Edges{
		{{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
	for (const auto &Edge : Edges)
		AddDebugLine(Snapshot, Bounds.Corners[Edge[0]], Bounds.Corners[Edge[1]], Color, Category);
}

void AddCircle(SceneRenderSnapshot &Snapshot, const glm::vec3 Center, const glm::vec3 AxisX, const glm::vec3 AxisY, const glm::vec4 Color,
			   const SceneDebugLineCategory Category)
{
	constexpr uint32 SegmentCount = 32;
	glm::vec3 Previous = Center + AxisX;
	for (uint32 Segment = 1; Segment <= SegmentCount; ++Segment)
	{
		const float32 Angle = glm::two_pi<float32>() * static_cast<float32>(Segment) / static_cast<float32>(SegmentCount);
		const glm::vec3 Current = Center + AxisX * std::cos(Angle) + AxisY * std::sin(Angle);
		AddDebugLine(Snapshot, Previous, Current, Color, Category);
		Previous = Current;
	}
}

void AddCameraLines(SceneRenderSnapshot &Snapshot, const components::CObjectCameraComponent &Camera, const glm::mat4 &Transform)
{
	const glm::vec3 Origin = glm::vec3(Transform[3]);
	const glm::vec3 Right = glm::normalize(glm::vec3(Transform[0]));
	const glm::vec3 Up = glm::normalize(glm::vec3(Transform[1]));
	const glm::vec3 Forward = -glm::normalize(glm::vec3(Transform[2]));
	const float32 Distance = std::min(Camera.GetFarPlane(), 5.0f);
	const float32 HalfHeight = Camera.GetProjection() == components::CameraProjection::Perspective
								   ? std::tan(glm::radians(Camera.GetVerticalFieldOfViewDegrees()) * 0.5f) * Distance
								   : Camera.GetOrthographicHeight() * 0.5f;
	const float32 HalfWidth = HalfHeight * (16.0f / 9.0f);
	const glm::vec3 Center = Origin + Forward * Distance;
	const std::array Corners{Center - Right * HalfWidth - Up * HalfHeight, Center + Right * HalfWidth - Up * HalfHeight,
							 Center + Right * HalfWidth + Up * HalfHeight, Center - Right * HalfWidth + Up * HalfHeight};
	const glm::vec4 Color(0.35f, 0.75f, 1.0f, 1.0f);
	for (uint32 Index = 0; Index < Corners.size(); ++Index)
	{
		AddDebugLine(Snapshot, Origin, Corners[Index], Color, SceneDebugLineCategory::Camera);
		AddDebugLine(Snapshot, Corners[Index], Corners[(Index + 1U) % Corners.size()], Color, SceneDebugLineCategory::Camera);
	}
}

void AddSkeletonLines(SceneRenderSnapshot &Snapshot, const SceneMeshSnapshot &Mesh, SceneRenderSnapshotBuildScratch &Scratch)
{
	const SceneAnimationSnapshot *Animation = Mesh.Animation.has_value() ? &*Mesh.Animation : nullptr;
	Scratch.SkeletonNodeTransforms.resize(Mesh.Model->GetNodes().size());
	for (uint32 NodeIndex = 0; NodeIndex < Mesh.Model->GetNodes().size(); ++NodeIndex)
	{
		const resource::ModelNode &Node = Mesh.Model->GetNodes()[NodeIndex];
		const glm::mat4 Parent =
			Node.ParentIndex == resource::InvalidModelNodeIndex ? Mesh.ObjectTransform : Scratch.SkeletonNodeTransforms[Node.ParentIndex];
		Scratch.SkeletonNodeTransforms[NodeIndex] = Parent * Node.LocalTransform;
	}
	for (const resource::ModelMeshInstance &Instance : Mesh.Model->GetMeshInstances())
	{
		if (Instance.NodeIndex >= Scratch.SkeletonNodeTransforms.size())
			continue;
		auto MeshAsset = Instance.Mesh.TryPin();
		if (MeshAsset == nullptr || MeshAsset->GetKind() != resource::MeshKind::Skeletal)
			continue;
		const auto &SkeletalMesh = static_cast<const resource::SkeletalMeshAsset &>(*MeshAsset);
		auto Skeleton = SkeletalMesh.GetSkeleton().TryPin();
		if (Skeleton == nullptr)
			continue;
		std::span<const glm::mat4> SkinPose;
		if (Animation != nullptr)
		{
			const auto State = std::ranges::find(Animation->RigStates, SkeletalMesh.GetSkeleton().GetID(),
												 &components::AnimationRigRuntimeState::Skeleton);
			if (State != Animation->RigStates.end())
				SkinPose = State->CurrentPose;
		}
		Scratch.SkeletonReferenceGlobal.clear();
		if (SkinPose.size() != Skeleton->GetJoints().size())
		{
			Scratch.SkeletonReferenceGlobal.resize(Skeleton->GetJoints().size());
			for (uint32 JointIndex = 0; JointIndex < Skeleton->GetJoints().size(); ++JointIndex)
			{
				const resource::SkeletonJoint &Joint = Skeleton->GetJoints()[JointIndex];
				Scratch.SkeletonReferenceGlobal[JointIndex] =
					(Joint.ParentIndex == resource::InvalidJointIndex ? glm::mat4(1.0f)
																	  : Scratch.SkeletonReferenceGlobal[Joint.ParentIndex]) *
					Joint.ReferenceLocalTransform;
			}
		}
		Scratch.SkeletonJointPositions.resize(Skeleton->GetJoints().size());
		for (uint32 JointIndex = 0; JointIndex < Skeleton->GetJoints().size(); ++JointIndex)
		{
			const resource::SkeletonJoint &Joint = Skeleton->GetJoints()[JointIndex];
			const glm::mat4 GlobalPose = Scratch.SkeletonReferenceGlobal.empty()
											 ? SkinPose[JointIndex] * glm::inverse(Joint.InverseBindMatrix)
											 : Scratch.SkeletonReferenceGlobal[JointIndex];
			Scratch.SkeletonJointPositions[JointIndex] =
				glm::vec3(Scratch.SkeletonNodeTransforms[Instance.NodeIndex] * GlobalPose * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		}
		const glm::vec4 Color(0.9f, 0.65f, 0.15f, 1.0f);
		for (uint32 JointIndex = 0; JointIndex < Skeleton->GetJoints().size(); ++JointIndex)
		{
			const uint32 Parent = Skeleton->GetJoints()[JointIndex].ParentIndex;
			if (Parent != resource::InvalidJointIndex)
				AddDebugLine(Snapshot, Scratch.SkeletonJointPositions[Parent], Scratch.SkeletonJointPositions[JointIndex], Color,
							 SceneDebugLineCategory::Skeleton);
		}
	}
}

class ObjectRenderability final
{
  public:
	ObjectRenderability(const world::Scene::ReadAccess &Access, const bool RespectEditorVisibility,
						SceneRenderSnapshotBuildScratch &Scratch)
		: Objects(&Scratch.Objects), Parents(&Scratch.Parents), LocalVisible(&Scratch.LocalVisible), States(&Scratch.VisibilityStates),
		  Indices(&Scratch.Indices), Chain(&Scratch.VisibilityChain), IndexGeneration(&Scratch.IndexGeneration)
	{
		Access.ObjectsInto(*this->Objects);
		this->Parents->assign(this->Objects->size(), world::ObjectHandle{});
		this->LocalVisible->assign(this->Objects->size(), true);
		this->States->assign(this->Objects->size(), static_cast<uint8>(State::Unresolved));
		++*this->IndexGeneration;
		if (*this->IndexGeneration == 0)
		{
			this->Indices->clear();
			*this->IndexGeneration = 1;
		}
		this->Indices->reserve(this->Objects->size());
		for (uint32 Index = 0; Index < this->Objects->size(); ++Index)
		{
			const world::ObjectHandle Object = (*this->Objects)[Index];
			auto [IndexEntry, Inserted] = this->Indices->try_emplace(Object);
			if (!Inserted && IndexEntry->second.Generation == *this->IndexGeneration)
				throw std::logic_error("Render snapshot encountered a duplicate object handle");
			IndexEntry->second = {.Index = Index, .Generation = *this->IndexGeneration};
			const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Object);
			if (Identity.IsValid())
			{
				const components::CObjectIdentityComponent &Component = Access.Resolve(Identity);
				(*this->LocalVisible)[Index] = Component.IsEnabled() && (!RespectEditorVisibility || Component.IsEditorVisible());
			}
			const auto Hierarchy = Access.GetComponent<components::CObjectHierarchyComponent>(Object);
			if (Hierarchy.IsValid())
				(*this->Parents)[Index] = Access.Resolve(Hierarchy).GetParent();
		}
		if (this->Indices->size() > this->Objects->size() * 2U)
		{
			const uint64 PublishedGeneration = *this->IndexGeneration;
			std::erase_if(*this->Indices,
						  [PublishedGeneration](const auto &Entry) { return Entry.second.Generation != PublishedGeneration; });
		}
	}

	[[nodiscard]] bool IsRenderable(const world::ObjectHandle Object)
	{
		const auto Found = this->Indices->find(Object);
		if (Found == this->Indices->end() || Found->second.Generation != *this->IndexGeneration)
			throw std::out_of_range("Render snapshot component owner is not a live scene object");
		if ((*this->States)[Found->second.Index] != static_cast<uint8>(State::Unresolved))
			return (*this->States)[Found->second.Index] == static_cast<uint8>(State::Visible);

		this->Chain->clear();
		world::ObjectHandle Current = Object;
		bool Visible = true;
		while (Current.IsValid())
		{
			const auto CurrentEntry = this->Indices->find(Current);
			if (CurrentEntry == this->Indices->end() || CurrentEntry->second.Generation != *this->IndexGeneration)
				throw std::logic_error("Render snapshot hierarchy references a missing parent object");
			const uint32 Index = CurrentEntry->second.Index;
			if ((*this->States)[Index] == static_cast<uint8>(State::Visible) || (*this->States)[Index] == static_cast<uint8>(State::Hidden))
			{
				Visible = (*this->States)[Index] == static_cast<uint8>(State::Visible);
				break;
			}
			if ((*this->States)[Index] == static_cast<uint8>(State::Resolving))
				throw std::logic_error("Render snapshot hierarchy contains a parent cycle");
			(*this->States)[Index] = static_cast<uint8>(State::Resolving);
			this->Chain->push_back(Index);
			Current = (*this->Parents)[Index];
		}
		for (auto Entry = this->Chain->rbegin(); Entry != this->Chain->rend(); ++Entry)
		{
			Visible = Visible && (*this->LocalVisible)[*Entry];
			(*this->States)[*Entry] = static_cast<uint8>(Visible ? State::Visible : State::Hidden);
		}
		return (*this->States)[Found->second.Index] == static_cast<uint8>(State::Visible);
	}

  private:
	enum class State : uint8
	{
		Unresolved,
		Resolving,
		Visible,
		Hidden
	};

	std::vector<world::ObjectHandle> *Objects = nullptr;
	std::vector<world::ObjectHandle> *Parents = nullptr;
	std::vector<bool> *LocalVisible = nullptr;
	std::vector<uint8> *States = nullptr;
	std::unordered_map<world::ObjectHandle, SceneRenderSnapshotBuildScratch::ObjectIndexEntry, world::ObjectHandleHash> *Indices = nullptr;
	std::vector<uint32> *Chain = nullptr;
	uint64 *IndexGeneration = nullptr;
};
} // namespace

SceneRenderSnapshot SceneRenderSnapshotBuilder::Build(const world::Scene &Scene, const SceneRenderSnapshotBuildOptions Options)
{
	SceneRenderSnapshot Result;
	SceneRenderSnapshotBuildScratch Scratch;
	SceneRenderSnapshotBuilder::BuildInto(Scene, Result, Options, Scratch);
	return Result;
}

void SceneRenderSnapshotBuilder::BuildInto(const world::Scene &Scene, SceneRenderSnapshot &Result,
										   const SceneRenderSnapshotBuildOptions Options)
{
	SceneRenderSnapshotBuildScratch Scratch;
	SceneRenderSnapshotBuilder::BuildInto(Scene, Result, Options, Scratch);
}

void SceneRenderSnapshotBuilder::BuildInto(const world::Scene &Scene, SceneRenderSnapshot &Result,
										   const SceneRenderSnapshotBuildOptions Options, SceneRenderSnapshotBuildScratch &Scratch)
{
	Result.SceneID = Scene.GetID();
	Result.ObjectCount = static_cast<uint32>(Scene.GetObjectCount());
	Result.DirectionalLights.clear();
	Result.PointLights.clear();
	Result.SpotLights.clear();
	Result.DebugLines.clear();
	Result.DebugBounds.clear();
	const auto Access = Scene.Read();
	world::SceneTransformSnapshot::BuildInto(Access, Scratch.WorldTransforms, Scratch.WorldTransformScratch);
	ObjectRenderability Renderability(Access, Options.RespectEditorVisibility, Scratch);
	for (const components::CObjectDirectionalLightComponent &Component : Access.Components<components::CObjectDirectionalLightComponent>())
	{
		if (!Component.IsEnabled() || !Renderability.IsRenderable(Component.GetOwner()))
			continue;
		const glm::vec3 Radiance = Component.GetColor() * (Component.GetIlluminanceLux() / 100'000.0f);
		Result.DirectionalLights.emplace_back(Scratch.WorldTransforms.GetForward(Component.GetOwner()), glm::vec3(0.0f), Radiance, Radiance,
											  Component.GetShadowSettings().CastShadows,
											  CaptureShadowParameters(Component.GetShadowSettings()), Component.GetAngularDiameterDegrees(),
											  Component.GetCascadeCount(), Component.GetCascadeDistributionExponent());
		if (Options.IncludeLights)
		{
			const glm::vec3 Origin = Scratch.WorldTransforms.GetPosition(Component.GetOwner());
			const glm::vec3 Direction = Scratch.WorldTransforms.GetForward(Component.GetOwner());
			AddDebugLine(Result, Origin, Origin + Direction * 3.0f, glm::vec4(1.0f, 0.9f, 0.25f, 1.0f), SceneDebugLineCategory::Light);
		}
	}
	for (const components::CObjectPointLightComponent &Component : Access.Components<components::CObjectPointLightComponent>())
	{
		if (!Component.IsEnabled() || !Renderability.IsRenderable(Component.GetOwner()))
			continue;
		const glm::vec3 Radiance = Component.GetColor() * (Component.GetLuminousPowerLumens() / (4.0f * glm::pi<float32>()));
		Result.PointLights.emplace_back(Scratch.WorldTransforms.GetPosition(Component.GetOwner()), glm::vec3(0.0f), Radiance, Radiance,
										1.0f, 0.0f, QuadraticForRange(Radiance, Component.GetRange()),
										Component.GetShadowSettings().CastShadows, CaptureShadowParameters(Component.GetShadowSettings()));
		if (Options.IncludeLights)
		{
			const glm::vec3 Position = Scratch.WorldTransforms.GetPosition(Component.GetOwner());
			const float32 Radius = Component.GetRange();
			const glm::vec4 Color(Component.GetColor(), 1.0f);
			AddCircle(Result, Position, glm::vec3(Radius, 0.0f, 0.0f), glm::vec3(0.0f, Radius, 0.0f), Color, SceneDebugLineCategory::Light);
			AddCircle(Result, Position, glm::vec3(Radius, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, Radius), Color, SceneDebugLineCategory::Light);
			AddCircle(Result, Position, glm::vec3(0.0f, Radius, 0.0f), glm::vec3(0.0f, 0.0f, Radius), Color, SceneDebugLineCategory::Light);
		}
	}
	for (const components::CObjectSpotLightComponent &Component : Access.Components<components::CObjectSpotLightComponent>())
	{
		if (!Component.IsEnabled() || !Renderability.IsRenderable(Component.GetOwner()))
			continue;
		const float32 OuterCosine = glm::cos(glm::radians(Component.GetOuterConeDegrees()));
		const float32 SolidAngle = std::max(2.0f * glm::pi<float32>() * (1.0f - OuterCosine), 0.0001f);
		const glm::vec3 Radiance = Component.GetColor() * (Component.GetLuminousPowerLumens() / SolidAngle);
		Result.SpotLights.emplace_back(Scratch.WorldTransforms.GetPosition(Component.GetOwner()),
									   Scratch.WorldTransforms.GetForward(Component.GetOwner()),
									   glm::cos(glm::radians(Component.GetInnerConeDegrees())), OuterCosine, glm::vec3(0.0f), Radiance,
									   Radiance, 1.0f, 0.0f, QuadraticForRange(Radiance, Component.GetRange()),
									   Component.GetShadowSettings().CastShadows, CaptureShadowParameters(Component.GetShadowSettings()));
		if (Options.IncludeLights)
		{
			const glm::vec3 Position = Scratch.WorldTransforms.GetPosition(Component.GetOwner());
			const glm::vec3 Direction = Scratch.WorldTransforms.GetForward(Component.GetOwner());
			const glm::vec3 Up = glm::abs(Direction.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			const glm::vec3 Right = glm::normalize(glm::cross(Direction, Up));
			const glm::vec3 ConeUp = glm::normalize(glm::cross(Right, Direction));
			const float32 Radius = std::tan(glm::radians(Component.GetOuterConeDegrees())) * Component.GetRange();
			const glm::vec3 End = Position + Direction * Component.GetRange();
			const glm::vec4 Color(Component.GetColor(), 1.0f);
			AddCircle(Result, End, Right * Radius, ConeUp * Radius, Color, SceneDebugLineCategory::Light);
			for (const glm::vec3 Offset : std::array{Right * Radius, -Right * Radius, ConeUp * Radius, -ConeUp * Radius})
				AddDebugLine(Result, Position, End + Offset, Color, SceneDebugLineCategory::Light);
		}
	}
	if (Options.IncludeCameras)
	{
		for (const components::CObjectCameraComponent &Component : Access.Components<components::CObjectCameraComponent>())
		{
			if (Renderability.IsRenderable(Component.GetOwner()))
				AddCameraLines(Result, Component, Scratch.WorldTransforms.GetMatrix(Component.GetOwner()));
		}
	}
	usize MeshCount = 0;
	for (const components::CObjectMeshComponent &Component : Access.Components<components::CObjectMeshComponent>())
	{
		if (!Component.IsEnabled() || !HasFlag(Component.GetVisibility(), components::MeshVisibilityFlags::Visible) ||
			!Renderability.IsRenderable(Component.GetOwner()))
			continue;
		resource::AssetPtr<resource::ModelAsset> Model = Component.GetModel().TryPin();
		if (Model == nullptr)
			continue;
		if (MeshCount == Result.Meshes.size())
			Result.Meshes.emplace_back();
		SceneMeshSnapshot &Mesh = Result.Meshes[MeshCount++];
		Mesh.Owner = Component.GetOwner();
		Mesh.Model = std::move(Model);
		Mesh.ObjectTransform = Scratch.WorldTransforms.GetMatrix(Component.GetOwner());
		Mesh.LODPolicy = Component.GetLODPolicy();
		const auto MaterialOverrides = Component.GetMaterialOverrides();
		Mesh.MaterialOverrides.assign(MaterialOverrides.begin(), MaterialOverrides.end());
		Mesh.Visibility = Component.GetVisibility();
		const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Component.GetOwner());
		Mesh.Mobility = Identity.IsValid() ? Access.Resolve(Identity).GetMobility() : components::ObjectMobility::Movable;
		Mesh.RenderLayerMask = Component.GetRenderLayerMask();
		Mesh.ModelPublishedGeneration = Component.GetModel().GetPublishedGeneration();
		const auto Animation = Access.GetComponent<components::CObjectAnimationComponent>(Component.GetOwner());
		if (Animation.IsValid())
		{
			const components::CObjectAnimationComponent &Source = Access.Resolve(Animation);
			const auto RigStates = Source.GetRigStates();
			const auto MorphWeights = Source.GetMorphWeights();
			if (!Mesh.Animation.has_value())
				Mesh.Animation.emplace();
			Mesh.Animation->RigStates.assign(RigStates.begin(), RigStates.end());
			Mesh.Animation->MorphWeights.assign(MorphWeights.begin(), MorphWeights.end());
		}
		else
		{
			Mesh.Animation.reset();
		}
		if (Options.IncludeBounds)
		{
			const SceneDebugBounds Bounds{.Owner = Mesh.Owner, .Corners = TransformBounds(Mesh.Model->GetBounds(), Mesh.ObjectTransform)};
			Result.DebugBounds.push_back(Bounds);
			AddBoundsLines(Result, Bounds, glm::vec4(0.2f, 0.8f, 1.0f, 1.0f), SceneDebugLineCategory::Bounds);
		}
		if (Options.IncludeSkeletons)
			AddSkeletonLines(Result, Mesh, Scratch);
	}
	Result.Meshes.resize(MeshCount);
}
} // namespace pipeline::render
