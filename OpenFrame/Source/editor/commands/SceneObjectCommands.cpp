#include "SceneObjectCommands.h"

#include "Source/component/object/CObjectAnimationComponent.h"
#include "Source/component/object/CObjectBehaviorComponent.h"
#include "Source/component/object/CObjectCameraComponent.h"
#include "Source/component/object/CObjectHierarchyComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectLightComponents.h"
#include "Source/component/object/CObjectMeshComponent.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/editor/document/SceneDocument.h"
#include "Source/editor/hierarchy/SceneHierarchy.h"
#include "Source/scene/SceneTransformSnapshot.h"
#include "Source/scene/TransformMath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace editor::commands
{
namespace
{
void SetSelection(document::SceneDocument &Document, const std::vector<util::UUID> &Objects)
{
	Document.GetSelection().Clear();
	for (const util::UUID &ID : Objects)
	{
		if (Document.GetScene().FindObject(ID).IsValid())
			Document.GetSelection().Add(ID);
	}
}

struct CameraState final
{
	components::CameraProjection Projection = components::CameraProjection::Perspective;
	float32 VerticalFieldOfViewDegrees = 60.0f;
	float32 OrthographicHeight = 10.0f;
	float32 NearPlane = 0.05f;
	float32 FarPlane = 100'000.0f;
	float32 ExposureCompensation = 0.0f;
	bool Primary = false;
	bool TemporalJitterEnabled = true;
};

struct MeshState final
{
	resource::AssetHandle<resource::ModelAsset> Model;
	std::vector<components::MeshMaterialOverride> MaterialOverrides;
	components::MeshVisibilityFlags Visibility = components::MeshVisibilityFlags::None;
	components::MeshLODPolicy LODPolicy;
	uint32 RenderLayerMask = 0;
};

struct AnimationState final
{
	resource::AssetHandle<resource::AnimationGraphAsset> Graph;
	std::vector<components::AnimationParameterValue> Parameters;
	std::vector<components::AnimationMorphWeight> MorphWeights;
	std::vector<resource::AssetHandle<resource::RetargetProfileAsset>> RetargetProfiles;
	components::AnimationUpdateMode UpdateMode = components::AnimationUpdateMode::VisibleOnly;
	bool RootMotionEnabled = false;
};

struct PointLightState final
{
	glm::vec3 Color{1.0f};
	float32 LuminousPowerLumens = 0.0f;
	float32 Range = 0.0f;
	float32 SourceRadius = 0.0f;
	components::LightShadowSettings Shadows;
};

struct SpotLightState final
{
	glm::vec3 Color{1.0f};
	float32 LuminousPowerLumens = 0.0f;
	float32 Range = 0.0f;
	float32 InnerConeDegrees = 0.0f;
	float32 OuterConeDegrees = 0.0f;
	components::LightShadowSettings Shadows;
};

struct DirectionalLightState final
{
	glm::vec3 Color{1.0f};
	float32 IlluminanceLux = 0.0f;
	float32 AngularDiameterDegrees = 0.0f;
	uint32 CascadeCount = 0;
	float32 CascadeDistributionExponent = 0.0f;
	components::LightShadowSettings Shadows;
};

struct ObjectState final
{
	util::UUID ID;
	util::UUID Parent;
	string Name;
	std::vector<string> Tags;
	uint32 SiblingOrder = 0;
	uint32 Depth = 0;
	components::ObjectMobility Mobility = components::ObjectMobility::Movable;
	bool Enabled = true;
	bool EditorVisible = true;
	bool Locked = false;
	glm::vec3 Position{0.0f};
	glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 Scale{1.0f};
	std::optional<CameraState> Camera;
	std::optional<MeshState> Mesh;
	std::optional<AnimationState> Animation;
	std::optional<std::vector<components::BehaviorInstance>> Behaviors;
	std::optional<PointLightState> PointLight;
	std::optional<SpotLightState> SpotLight;
	std::optional<DirectionalLightState> DirectionalLight;
};

using ComponentArchiveState = std::variant<CameraState, MeshState, AnimationState, std::vector<components::BehaviorInstance>,
										   PointLightState, SpotLightState, DirectionalLightState>;

[[nodiscard]] ComponentArchiveState CaptureComponent(world::Scene &Scene, const world::ObjectHandle Object, const uint32 ComponentType)
{
	if (ComponentType == components::CObjectCameraComponent::TypeID)
	{
		const auto Handle = Scene.GetComponent<components::CObjectCameraComponent>(Object);
		auto Access = Scene.Read();
		const components::CObjectCameraComponent &Component = Access.Resolve(Handle);
		return CameraState{.Projection = Component.GetProjection(),
						   .VerticalFieldOfViewDegrees = Component.GetVerticalFieldOfViewDegrees(),
						   .OrthographicHeight = Component.GetOrthographicHeight(),
						   .NearPlane = Component.GetNearPlane(),
						   .FarPlane = Component.GetFarPlane(),
						   .ExposureCompensation = Component.GetExposureCompensation(),
						   .Primary = Component.IsPrimary(),
						   .TemporalJitterEnabled = Component.IsTemporalJitterEnabled()};
	}
	if (ComponentType == components::CObjectMeshComponent::TypeID)
	{
		const auto Handle = Scene.GetComponent<components::CObjectMeshComponent>(Object);
		auto Access = Scene.Read();
		const components::CObjectMeshComponent &Component = Access.Resolve(Handle);
		return MeshState{.Model = Component.GetModel(),
						 .MaterialOverrides = {Component.GetMaterialOverrides().begin(), Component.GetMaterialOverrides().end()},
						 .Visibility = Component.GetVisibility(),
						 .LODPolicy = Component.GetLODPolicy(),
						 .RenderLayerMask = Component.GetRenderLayerMask()};
	}
	if (ComponentType == components::CObjectAnimationComponent::TypeID)
	{
		const auto Handle = Scene.GetComponent<components::CObjectAnimationComponent>(Object);
		auto Access = Scene.Read();
		const components::CObjectAnimationComponent &Component = Access.Resolve(Handle);
		return AnimationState{.Graph = Component.GetGraph(),
							  .Parameters = {Component.GetParameters().begin(), Component.GetParameters().end()},
							  .MorphWeights = {Component.GetMorphWeights().begin(), Component.GetMorphWeights().end()},
							  .RetargetProfiles = {Component.GetRetargetProfiles().begin(), Component.GetRetargetProfiles().end()},
							  .UpdateMode = Component.GetUpdateMode(),
							  .RootMotionEnabled = Component.IsRootMotionEnabled()};
	}
	if (ComponentType == components::CObjectBehaviorComponent::TypeID)
	{
		const auto Handle = Scene.GetComponent<components::CObjectBehaviorComponent>(Object);
		auto Access = Scene.Read();
		const auto &Behaviors = Access.Resolve(Handle).GetBehaviors();
		return std::vector<components::BehaviorInstance>(Behaviors.begin(), Behaviors.end());
	}
	if (ComponentType == components::CObjectPointLightComponent::TypeID)
	{
		const auto Handle = Scene.GetComponent<components::CObjectPointLightComponent>(Object);
		auto Access = Scene.Read();
		const components::CObjectPointLightComponent &Component = Access.Resolve(Handle);
		return PointLightState{.Color = Component.GetColor(),
							   .LuminousPowerLumens = Component.GetLuminousPowerLumens(),
							   .Range = Component.GetRange(),
							   .SourceRadius = Component.GetSourceRadius(),
							   .Shadows = Component.GetShadowSettings()};
	}
	if (ComponentType == components::CObjectSpotLightComponent::TypeID)
	{
		const auto Handle = Scene.GetComponent<components::CObjectSpotLightComponent>(Object);
		auto Access = Scene.Read();
		const components::CObjectSpotLightComponent &Component = Access.Resolve(Handle);
		return SpotLightState{.Color = Component.GetColor(),
							  .LuminousPowerLumens = Component.GetLuminousPowerLumens(),
							  .Range = Component.GetRange(),
							  .InnerConeDegrees = Component.GetInnerConeDegrees(),
							  .OuterConeDegrees = Component.GetOuterConeDegrees(),
							  .Shadows = Component.GetShadowSettings()};
	}
	if (ComponentType == components::CObjectDirectionalLightComponent::TypeID)
	{
		const auto Handle = Scene.GetComponent<components::CObjectDirectionalLightComponent>(Object);
		auto Access = Scene.Read();
		const components::CObjectDirectionalLightComponent &Component = Access.Resolve(Handle);
		return DirectionalLightState{.Color = Component.GetColor(),
									 .IlluminanceLux = Component.GetIlluminanceLux(),
									 .AngularDiameterDegrees = Component.GetAngularDiameterDegrees(),
									 .CascadeCount = Component.GetCascadeCount(),
									 .CascadeDistributionExponent = Component.GetCascadeDistributionExponent(),
									 .Shadows = Component.GetShadowSettings()};
	}
	throw std::invalid_argument("Component type cannot be archived by an editor component command");
}

void AddDefaultComponent(world::Scene &Scene, const world::ObjectHandle Object, const uint32 ComponentType)
{
	if (ComponentType == components::CObjectCameraComponent::TypeID)
		(void)Scene.AddComponent<components::CObjectCameraComponent>(Object);
	else if (ComponentType == components::CObjectBehaviorComponent::TypeID)
		(void)Scene.AddComponent<components::CObjectBehaviorComponent>(Object);
	else if (ComponentType == components::CObjectPointLightComponent::TypeID)
		(void)Scene.AddComponent<components::CObjectPointLightComponent>(Object);
	else if (ComponentType == components::CObjectSpotLightComponent::TypeID)
		(void)Scene.AddComponent<components::CObjectSpotLightComponent>(Object);
	else if (ComponentType == components::CObjectDirectionalLightComponent::TypeID)
		(void)Scene.AddComponent<components::CObjectDirectionalLightComponent>(Object);
	else
		throw std::invalid_argument("Component type requires an explicit asset or cannot be added by the default component command");
}

void RemoveComponent(world::Scene &Scene, const world::ObjectHandle Object, const uint32 ComponentType)
{
	if (ComponentType == components::CObjectCameraComponent::TypeID)
		Scene.RemoveComponent<components::CObjectCameraComponent>(Object);
	else if (ComponentType == components::CObjectMeshComponent::TypeID)
		Scene.RemoveComponent<components::CObjectMeshComponent>(Object);
	else if (ComponentType == components::CObjectAnimationComponent::TypeID)
		Scene.RemoveComponent<components::CObjectAnimationComponent>(Object);
	else if (ComponentType == components::CObjectBehaviorComponent::TypeID)
		Scene.RemoveComponent<components::CObjectBehaviorComponent>(Object);
	else if (ComponentType == components::CObjectPointLightComponent::TypeID)
		Scene.RemoveComponent<components::CObjectPointLightComponent>(Object);
	else if (ComponentType == components::CObjectSpotLightComponent::TypeID)
		Scene.RemoveComponent<components::CObjectSpotLightComponent>(Object);
	else if (ComponentType == components::CObjectDirectionalLightComponent::TypeID)
		Scene.RemoveComponent<components::CObjectDirectionalLightComponent>(Object);
	else
		throw std::invalid_argument("Core identity, transform, and hierarchy components cannot be removed");
}

void RestoreComponent(world::Scene &Scene, const world::ObjectHandle Object, const ComponentArchiveState &State)
{
	std::visit(
		[&Scene, Object]<typename StateType>(const StateType &Value)
		{
			if constexpr (std::same_as<StateType, CameraState>)
			{
				const auto Handle = Scene.AddComponent<components::CObjectCameraComponent>(Object);
				auto Access = Scene.Write();
				components::CObjectCameraComponent &Component = Access.Resolve(Handle);
				Component.SetProjection(Value.Projection);
				Component.SetVerticalFieldOfViewDegrees(Value.VerticalFieldOfViewDegrees);
				Component.SetOrthographicHeight(Value.OrthographicHeight);
				Component.SetClipPlanes(Value.NearPlane, Value.FarPlane);
				Component.SetExposureCompensation(Value.ExposureCompensation);
				Component.SetPrimary(Value.Primary);
				Component.SetTemporalJitterEnabled(Value.TemporalJitterEnabled);
			}
			else if constexpr (std::same_as<StateType, MeshState>)
			{
				const auto Handle = Scene.AddComponent<components::CObjectMeshComponent>(Object, Value.Model);
				auto Access = Scene.Write();
				components::CObjectMeshComponent &Component = Access.Resolve(Handle);
				Component.SetVisibility(Value.Visibility);
				Component.SetLODPolicy(Value.LODPolicy);
				Component.SetRenderLayerMask(Value.RenderLayerMask);
				for (const components::MeshMaterialOverride &Override : Value.MaterialOverrides)
					Component.SetMaterialOverride(Override.MeshInstance, Override.MaterialSlot, Override.Material);
			}
			else if constexpr (std::same_as<StateType, AnimationState>)
			{
				const auto Handle = Scene.AddComponent<components::CObjectAnimationComponent>(Object, Value.Graph);
				auto Access = Scene.Write();
				components::CObjectAnimationComponent &Component = Access.Resolve(Handle);
				for (const components::AnimationParameterValue &Parameter : Value.Parameters)
					Component.SetParameter(Parameter.ID, Parameter.Type, Parameter.Value);
				for (const components::AnimationMorphWeight &Morph : Value.MorphWeights)
					Component.SetMorphWeight(Morph.Target, Morph.Weight);
				for (const resource::AssetHandle<resource::RetargetProfileAsset> &Profile : Value.RetargetProfiles)
					Component.SetRetargetProfile(Profile);
				Component.SetUpdateMode(Value.UpdateMode);
				Component.SetRootMotionEnabled(Value.RootMotionEnabled);
			}
			else if constexpr (std::same_as<StateType, std::vector<components::BehaviorInstance>>)
			{
				const auto Handle = Scene.AddComponent<components::CObjectBehaviorComponent>(Object);
				auto Access = Scene.Write();
				Access.Resolve(Handle).ReplaceBehaviors(Value);
			}
			else if constexpr (std::same_as<StateType, PointLightState>)
			{
				const auto Handle = Scene.AddComponent<components::CObjectPointLightComponent>(Object);
				auto Access = Scene.Write();
				components::CObjectPointLightComponent &Component = Access.Resolve(Handle);
				Component.SetColor(Value.Color);
				Component.SetLuminousPowerLumens(Value.LuminousPowerLumens);
				Component.SetRange(Value.Range);
				Component.SetSourceRadius(Value.SourceRadius);
				Component.GetShadowSettings() = Value.Shadows;
			}
			else if constexpr (std::same_as<StateType, SpotLightState>)
			{
				const auto Handle = Scene.AddComponent<components::CObjectSpotLightComponent>(Object);
				auto Access = Scene.Write();
				components::CObjectSpotLightComponent &Component = Access.Resolve(Handle);
				Component.SetColor(Value.Color);
				Component.SetLuminousPowerLumens(Value.LuminousPowerLumens);
				Component.SetRange(Value.Range);
				Component.SetConeAngles(Value.InnerConeDegrees, Value.OuterConeDegrees);
				Component.GetShadowSettings() = Value.Shadows;
			}
			else if constexpr (std::same_as<StateType, DirectionalLightState>)
			{
				const auto Handle = Scene.AddComponent<components::CObjectDirectionalLightComponent>(Object);
				auto Access = Scene.Write();
				components::CObjectDirectionalLightComponent &Component = Access.Resolve(Handle);
				Component.SetColor(Value.Color);
				Component.SetIlluminanceLux(Value.IlluminanceLux);
				Component.SetAngularDiameterDegrees(Value.AngularDiameterDegrees);
				Component.SetCascadeCount(Value.CascadeCount);
				Component.SetCascadeDistributionExponent(Value.CascadeDistributionExponent);
				Component.GetShadowSettings() = Value.Shadows;
			}
		},
		State);
}

[[nodiscard]] bool HasEditableComponent(world::Scene &Scene, const world::ObjectHandle Object, const uint32 ComponentType)
{
	if (ComponentType == components::CObjectCameraComponent::TypeID)
		return Scene.GetComponent<components::CObjectCameraComponent>(Object).IsValid();
	if (ComponentType == components::CObjectMeshComponent::TypeID)
		return Scene.GetComponent<components::CObjectMeshComponent>(Object).IsValid();
	if (ComponentType == components::CObjectAnimationComponent::TypeID)
		return Scene.GetComponent<components::CObjectAnimationComponent>(Object).IsValid();
	if (ComponentType == components::CObjectBehaviorComponent::TypeID)
		return Scene.GetComponent<components::CObjectBehaviorComponent>(Object).IsValid();
	if (ComponentType == components::CObjectPointLightComponent::TypeID)
		return Scene.GetComponent<components::CObjectPointLightComponent>(Object).IsValid();
	if (ComponentType == components::CObjectSpotLightComponent::TypeID)
		return Scene.GetComponent<components::CObjectSpotLightComponent>(Object).IsValid();
	if (ComponentType == components::CObjectDirectionalLightComponent::TypeID)
		return Scene.GetComponent<components::CObjectDirectionalLightComponent>(Object).IsValid();
	throw std::invalid_argument("Component type is not editable through component commands");
}

[[nodiscard]] util::UUID GetPersistentID(const world::Scene::ReadAccess &Access, const world::ObjectHandle Object)
{
	const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Object);
	if (!Identity.IsValid())
		throw std::logic_error("Editor object archive encountered an object without an identity component");
	return Access.Resolve(Identity).GetPersistentID();
}

[[nodiscard]] ObjectState CaptureObject(const world::Scene::ReadAccess &Access, const hierarchy::SceneHierarchySnapshot &Hierarchy,
										const hierarchy::SceneHierarchyRow &Row)
{
	const auto IdentityHandle = Access.GetComponent<components::CObjectIdentityComponent>(Row.Object);
	const auto TransformHandle = Access.GetComponent<components::CObjectTransformComponent>(Row.Object);
	const auto HierarchyHandle = Access.GetComponent<components::CObjectHierarchyComponent>(Row.Object);
	if (!IdentityHandle.IsValid() || !TransformHandle.IsValid() || !HierarchyHandle.IsValid())
		throw std::logic_error("Editor object archive requires identity, transform, and hierarchy components");
	const components::CObjectIdentityComponent &Identity = Access.Resolve(IdentityHandle);
	const components::CObjectTransformComponent &Transform = Access.Resolve(TransformHandle);
	const components::CObjectHierarchyComponent &HierarchyComponent = Access.Resolve(HierarchyHandle);
	ObjectState Result{.ID = Identity.GetPersistentID(),
					   .Name = Identity.GetName(),
					   .Tags = {Identity.GetTags().begin(), Identity.GetTags().end()},
					   .SiblingOrder = HierarchyComponent.GetSiblingOrder(),
					   .Depth = Row.Depth,
					   .Mobility = Identity.GetMobility(),
					   .Enabled = Identity.IsEnabled(),
					   .EditorVisible = Identity.IsEditorVisible(),
					   .Locked = Identity.IsLocked(),
					   .Position = Transform.GetPosition(),
					   .Rotation = Transform.GetRotation(),
					   .Scale = Transform.GetScale()};
	if (Row.ParentRow != hierarchy::InvalidHierarchyRow)
		Result.Parent = Hierarchy.Rows[Row.ParentRow].PersistentID;

	if (const auto Handle = Access.GetComponent<components::CObjectCameraComponent>(Row.Object); Handle.IsValid())
	{
		const components::CObjectCameraComponent &Component = Access.Resolve(Handle);
		Result.Camera = CameraState{.Projection = Component.GetProjection(),
									.VerticalFieldOfViewDegrees = Component.GetVerticalFieldOfViewDegrees(),
									.OrthographicHeight = Component.GetOrthographicHeight(),
									.NearPlane = Component.GetNearPlane(),
									.FarPlane = Component.GetFarPlane(),
									.ExposureCompensation = Component.GetExposureCompensation(),
									.Primary = Component.IsPrimary(),
									.TemporalJitterEnabled = Component.IsTemporalJitterEnabled()};
	}
	if (const auto Handle = Access.GetComponent<components::CObjectMeshComponent>(Row.Object); Handle.IsValid())
	{
		const components::CObjectMeshComponent &Component = Access.Resolve(Handle);
		Result.Mesh = MeshState{.Model = Component.GetModel(),
								.MaterialOverrides = {Component.GetMaterialOverrides().begin(), Component.GetMaterialOverrides().end()},
								.Visibility = Component.GetVisibility(),
								.LODPolicy = Component.GetLODPolicy(),
								.RenderLayerMask = Component.GetRenderLayerMask()};
	}
	if (const auto Handle = Access.GetComponent<components::CObjectAnimationComponent>(Row.Object); Handle.IsValid())
	{
		const components::CObjectAnimationComponent &Component = Access.Resolve(Handle);
		Result.Animation =
			AnimationState{.Graph = Component.GetGraph(),
						   .Parameters = {Component.GetParameters().begin(), Component.GetParameters().end()},
						   .MorphWeights = {Component.GetMorphWeights().begin(), Component.GetMorphWeights().end()},
						   .RetargetProfiles = {Component.GetRetargetProfiles().begin(), Component.GetRetargetProfiles().end()},
						   .UpdateMode = Component.GetUpdateMode(),
						   .RootMotionEnabled = Component.IsRootMotionEnabled()};
	}
	if (const auto Handle = Access.GetComponent<components::CObjectBehaviorComponent>(Row.Object); Handle.IsValid())
		Result.Behaviors = Access.Resolve(Handle).GetBehaviors();
	if (const auto Handle = Access.GetComponent<components::CObjectPointLightComponent>(Row.Object); Handle.IsValid())
	{
		const components::CObjectPointLightComponent &Component = Access.Resolve(Handle);
		Result.PointLight = PointLightState{.Color = Component.GetColor(),
											.LuminousPowerLumens = Component.GetLuminousPowerLumens(),
											.Range = Component.GetRange(),
											.SourceRadius = Component.GetSourceRadius(),
											.Shadows = Component.GetShadowSettings()};
	}
	if (const auto Handle = Access.GetComponent<components::CObjectSpotLightComponent>(Row.Object); Handle.IsValid())
	{
		const components::CObjectSpotLightComponent &Component = Access.Resolve(Handle);
		Result.SpotLight = SpotLightState{.Color = Component.GetColor(),
										  .LuminousPowerLumens = Component.GetLuminousPowerLumens(),
										  .Range = Component.GetRange(),
										  .InnerConeDegrees = Component.GetInnerConeDegrees(),
										  .OuterConeDegrees = Component.GetOuterConeDegrees(),
										  .Shadows = Component.GetShadowSettings()};
	}
	if (const auto Handle = Access.GetComponent<components::CObjectDirectionalLightComponent>(Row.Object); Handle.IsValid())
	{
		const components::CObjectDirectionalLightComponent &Component = Access.Resolve(Handle);
		Result.DirectionalLight = DirectionalLightState{.Color = Component.GetColor(),
														.IlluminanceLux = Component.GetIlluminanceLux(),
														.AngularDiameterDegrees = Component.GetAngularDiameterDegrees(),
														.CascadeCount = Component.GetCascadeCount(),
														.CascadeDistributionExponent = Component.GetCascadeDistributionExponent(),
														.Shadows = Component.GetShadowSettings()};
	}
	return Result;
}

class SceneObjectArchive final
{
  public:
	SceneObjectArchive(document::SceneDocument &Document, const std::vector<util::UUID> &Objects, const bool Duplicate)
	{
		if (Objects.empty())
			throw std::invalid_argument("Scene object command requires at least one object");
		const hierarchy::SceneHierarchySnapshot Hierarchy =
			hierarchy::SceneHierarchyBuilder::Build(Document.GetScene(), Document.GetRevision());
		std::unordered_set<util::UUID> Requested(Objects.begin(), Objects.end());
		for (const util::UUID &ID : Requested)
		{
			if (!Document.GetScene().FindObject(ID).IsValid())
				throw std::out_of_range("Scene object command target does not exist");
		}

		std::vector<uint32> RootRows;
		for (uint32 RowIndex = 0; RowIndex < Hierarchy.Rows.size(); ++RowIndex)
		{
			if (!Requested.contains(Hierarchy.Rows[RowIndex].PersistentID))
				continue;
			uint32 Parent = Hierarchy.Rows[RowIndex].ParentRow;
			bool AncestorRequested = false;
			while (Parent != hierarchy::InvalidHierarchyRow)
			{
				if (Requested.contains(Hierarchy.Rows[Parent].PersistentID))
				{
					AncestorRequested = true;
					break;
				}
				Parent = Hierarchy.Rows[Parent].ParentRow;
			}
			if (!AncestorRequested)
				RootRows.push_back(RowIndex);
		}
		auto Access = Document.GetScene().Read();
		for (const uint32 Root : RootRows)
		{
			const uint32 RootDepth = Hierarchy.Rows[Root].Depth;
			for (uint32 Row = Root; Row < Hierarchy.Rows.size() && (Row == Root || Hierarchy.Rows[Row].Depth > RootDepth); ++Row)
				this->Objects.push_back(CaptureObject(Access, Hierarchy, Hierarchy.Rows[Row]));
		}
		if (Duplicate)
			this->RemapForDuplicate();
		else
			this->CacheIDs();
	}

	[[nodiscard]] SceneObjectArchive CreateDuplicate() const
	{
		SceneObjectArchive Result(*this);
		Result.RemapForDuplicate();
		return Result;
	}

	[[nodiscard]] const std::vector<util::UUID> &GetIDs() const noexcept
	{
		return this->IDs;
	}

	[[nodiscard]] usize Size() const noexcept
	{
		return this->Objects.size();
	}

	[[nodiscard]] std::vector<resource::AssetID> GetMaterialOverrideAssetIDs() const
	{
		std::vector<resource::AssetID> Result;
		for (const ObjectState &Object : this->Objects)
		{
			if (!Object.Mesh.has_value())
				continue;
			for (const components::MeshMaterialOverride &Override : Object.Mesh->MaterialOverrides)
			{
				if (Override.Material)
					Result.push_back(Override.Material.GetID());
			}
		}
		std::ranges::sort(Result);
		Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
		return Result;
	}

	void Restore(document::SceneDocument &Document) const
	{
		const std::vector<util::UUID> PreviousSelection = Document.GetSelection().GetOrdered();
		std::unordered_set<util::UUID> Available;
		Available.reserve(Document.GetScene().GetObjectCount() + this->Objects.size());
		{
			auto Access = Document.GetScene().Read();
			for (const world::ObjectHandle Existing : Access.Objects())
				Available.emplace(GetPersistentID(Access, Existing));
		}
		for (const ObjectState &State : this->Objects)
		{
			if (Available.contains(State.ID))
				throw std::invalid_argument("Scene object archive identity is already present during restore");
			if (State.Parent.IsValid() && !Available.contains(State.Parent))
				throw std::out_of_range("Scene object archive parent is not available during restore");
			Available.emplace(State.ID);
		}
		std::vector<util::UUID> Created;
		Created.reserve(this->Objects.size());
		try
		{
			for (const ObjectState &State : this->Objects)
			{
				const world::ObjectHandle Parent =
					State.Parent.IsValid() ? Document.GetScene().FindObject(State.Parent) : world::ObjectHandle{};
				if (State.Parent.IsValid() && !Parent.IsValid())
					throw std::out_of_range("Scene object archive parent is not available during restore");
				const world::ObjectHandle Object = Document.CreateObject(State.Name, Parent, State.ID);
				Created.push_back(State.ID);
				RestoreComponents(Document.GetScene(), Object, State);
				if (Parent.IsValid())
					Document.GetScene().SetParent(Object, Parent, State.SiblingOrder);
			}
		}
		catch (...)
		{
			for (auto ID = Created.rbegin(); ID != Created.rend(); ++ID)
			{
				if (Document.GetScene().FindObject(*ID).IsValid())
					Document.DestroyObject(*ID);
			}
			SetSelection(Document, PreviousSelection);
			throw;
		}
	}

	void Destroy(document::SceneDocument &Document) const
	{
		for (const ObjectState &Object : this->Objects)
		{
			if (!Document.GetScene().FindObject(Object.ID).IsValid())
				throw std::out_of_range("Scene object archive target is missing during destruction");
		}
		for (auto Object = this->Objects.rbegin(); Object != this->Objects.rend(); ++Object)
		{
			Document.DestroyObject(Object->ID);
		}
	}

  private:
	void RemapForDuplicate()
	{
		std::unordered_set<util::UUID> InternalIDs;
		InternalIDs.reserve(this->Objects.size());
		for (const ObjectState &Object : this->Objects)
			InternalIDs.insert(Object.ID);

		std::unordered_map<util::UUID, util::UUID> Remap;
		Remap.reserve(this->Objects.size());
		for (ObjectState &Object : this->Objects)
		{
			const bool Root = !InternalIDs.contains(Object.Parent);
			const util::UUID Original = Object.ID;
			const util::UUID Replacement = util::UUID::GenerateRandomUUID();
			Remap.emplace(Original, Replacement);
			Object.ID = Replacement;
			if (Root)
				Object.Name += " Copy";
		}
		for (ObjectState &Object : this->Objects)
		{
			if (const auto Parent = Remap.find(Object.Parent); Parent != Remap.end())
				Object.Parent = Parent->second;
			if (!Object.Behaviors.has_value())
				continue;
			for (components::BehaviorInstance &Behavior : *Object.Behaviors)
			{
				Behavior.InstanceID = util::UUID::GenerateRandomUUID();
				for (auto &[Name, Value] : Behavior.Properties)
				{
					(void)Name;
					util::UUID *Reference = std::get_if<util::UUID>(&Value);
					if (Reference == nullptr)
						continue;
					if (const auto Replacement = Remap.find(*Reference); Replacement != Remap.end())
						*Reference = Replacement->second;
				}
			}
		}
		this->CacheIDs();
	}

	void CacheIDs()
	{
		this->IDs.clear();
		this->IDs.reserve(this->Objects.size());
		for (const ObjectState &Object : this->Objects)
			this->IDs.push_back(Object.ID);
	}

	static void RestoreComponents(world::Scene &Scene, const world::ObjectHandle Object, const ObjectState &State)
	{
		const auto IdentityHandle = Scene.GetComponent<components::CObjectIdentityComponent>(Object);
		const auto TransformHandle = Scene.GetComponent<components::CObjectTransformComponent>(Object);
		{
			auto Access = Scene.Write();
			components::CObjectIdentityComponent &Identity = Access.Resolve(IdentityHandle);
			Identity.SetTags(State.Tags);
			Identity.SetMobility(State.Mobility);
			Identity.SetEnabled(State.Enabled);
			Identity.SetEditorVisible(State.EditorVisible);
			Identity.SetLocked(State.Locked);
			Access.Resolve(TransformHandle).SetTransform(State.Position, State.Rotation, State.Scale);
		}
		if (State.Camera.has_value())
		{
			const auto Handle = Scene.AddComponent<components::CObjectCameraComponent>(Object);
			auto Access = Scene.Write();
			components::CObjectCameraComponent &Component = Access.Resolve(Handle);
			Component.SetProjection(State.Camera->Projection);
			Component.SetVerticalFieldOfViewDegrees(State.Camera->VerticalFieldOfViewDegrees);
			Component.SetOrthographicHeight(State.Camera->OrthographicHeight);
			Component.SetClipPlanes(State.Camera->NearPlane, State.Camera->FarPlane);
			Component.SetExposureCompensation(State.Camera->ExposureCompensation);
			Component.SetPrimary(State.Camera->Primary);
			Component.SetTemporalJitterEnabled(State.Camera->TemporalJitterEnabled);
		}
		if (State.Mesh.has_value())
		{
			const auto Handle = Scene.AddComponent<components::CObjectMeshComponent>(Object, State.Mesh->Model);
			auto Access = Scene.Write();
			components::CObjectMeshComponent &Component = Access.Resolve(Handle);
			Component.SetVisibility(State.Mesh->Visibility);
			Component.SetLODPolicy(State.Mesh->LODPolicy);
			Component.SetRenderLayerMask(State.Mesh->RenderLayerMask);
			for (const components::MeshMaterialOverride &Override : State.Mesh->MaterialOverrides)
				Component.SetMaterialOverride(Override.MeshInstance, Override.MaterialSlot, Override.Material);
		}
		if (State.Animation.has_value())
		{
			const auto Handle = Scene.AddComponent<components::CObjectAnimationComponent>(Object, State.Animation->Graph);
			auto Access = Scene.Write();
			components::CObjectAnimationComponent &Component = Access.Resolve(Handle);
			for (const components::AnimationParameterValue &Parameter : State.Animation->Parameters)
				Component.SetParameter(Parameter.ID, Parameter.Type, Parameter.Value);
			for (const components::AnimationMorphWeight &Morph : State.Animation->MorphWeights)
				Component.SetMorphWeight(Morph.MorphSet, Morph.Target, Morph.Weight);
			for (const resource::AssetHandle<resource::RetargetProfileAsset> &Profile : State.Animation->RetargetProfiles)
				Component.SetRetargetProfile(Profile);
			Component.SetUpdateMode(State.Animation->UpdateMode);
			Component.SetRootMotionEnabled(State.Animation->RootMotionEnabled);
		}
		if (State.Behaviors.has_value())
		{
			const auto Handle = Scene.AddComponent<components::CObjectBehaviorComponent>(Object);
			auto Access = Scene.Write();
			Access.Resolve(Handle).ReplaceBehaviors(*State.Behaviors);
		}
		if (State.PointLight.has_value())
		{
			const auto Handle = Scene.AddComponent<components::CObjectPointLightComponent>(Object);
			auto Access = Scene.Write();
			components::CObjectPointLightComponent &Component = Access.Resolve(Handle);
			Component.SetColor(State.PointLight->Color);
			Component.SetLuminousPowerLumens(State.PointLight->LuminousPowerLumens);
			Component.SetRange(State.PointLight->Range);
			Component.SetSourceRadius(State.PointLight->SourceRadius);
			Component.GetShadowSettings() = State.PointLight->Shadows;
		}
		if (State.SpotLight.has_value())
		{
			const auto Handle = Scene.AddComponent<components::CObjectSpotLightComponent>(Object);
			auto Access = Scene.Write();
			components::CObjectSpotLightComponent &Component = Access.Resolve(Handle);
			Component.SetColor(State.SpotLight->Color);
			Component.SetLuminousPowerLumens(State.SpotLight->LuminousPowerLumens);
			Component.SetRange(State.SpotLight->Range);
			Component.SetConeAngles(State.SpotLight->InnerConeDegrees, State.SpotLight->OuterConeDegrees);
			Component.GetShadowSettings() = State.SpotLight->Shadows;
		}
		if (State.DirectionalLight.has_value())
		{
			const auto Handle = Scene.AddComponent<components::CObjectDirectionalLightComponent>(Object);
			auto Access = Scene.Write();
			components::CObjectDirectionalLightComponent &Component = Access.Resolve(Handle);
			Component.SetColor(State.DirectionalLight->Color);
			Component.SetIlluminanceLux(State.DirectionalLight->IlluminanceLux);
			Component.SetAngularDiameterDegrees(State.DirectionalLight->AngularDiameterDegrees);
			Component.SetCascadeCount(State.DirectionalLight->CascadeCount);
			Component.SetCascadeDistributionExponent(State.DirectionalLight->CascadeDistributionExponent);
			Component.GetShadowSettings() = State.DirectionalLight->Shadows;
		}
	}

	std::vector<ObjectState> Objects;
	std::vector<util::UUID> IDs;
};
} // namespace

class SceneObjectSnapshot::Storage final
{
  public:
	Storage(document::SceneDocument &Document, const std::span<const util::UUID> Objects)
		: Value(Document, std::vector<util::UUID>(Objects.begin(), Objects.end()), false)
	{
	}

	SceneObjectArchive Value;
};

SceneObjectSnapshot SceneObjectSnapshot::Capture(document::SceneDocument &Document, const std::span<const util::UUID> Objects)
{
	return SceneObjectSnapshot(std::make_unique<Storage>(Document, Objects));
}

SceneObjectSnapshot::SceneObjectSnapshot(std::unique_ptr<Storage> State) noexcept : State(std::move(State))
{
}

SceneObjectSnapshot::SceneObjectSnapshot(const SceneObjectSnapshot &Other)
	: State(Other.State == nullptr ? nullptr : std::make_unique<Storage>(*Other.State))
{
}

SceneObjectSnapshot &SceneObjectSnapshot::operator=(const SceneObjectSnapshot &Other)
{
	if (this != &Other)
		this->State = Other.State == nullptr ? nullptr : std::make_unique<Storage>(*Other.State);
	return *this;
}

SceneObjectSnapshot::SceneObjectSnapshot(SceneObjectSnapshot &&Other) noexcept = default;
SceneObjectSnapshot &SceneObjectSnapshot::operator=(SceneObjectSnapshot &&Other) noexcept = default;
SceneObjectSnapshot::~SceneObjectSnapshot() = default;

bool SceneObjectSnapshot::Empty() const noexcept
{
	return this->State == nullptr || this->State->Value.Size() == 0;
}

usize SceneObjectSnapshot::GetObjectCount() const noexcept
{
	return this->State == nullptr ? 0 : this->State->Value.Size();
}

CreateObjectCommand::CreateObjectCommand(document::SceneDocument &Document, string Name, const util::UUID Parent)
	: Document(&Document), Name(std::move(Name)), Parent(Parent), PreviousSelection(Document.GetSelection().GetOrdered())
{
	if (this->Name.empty())
		throw std::invalid_argument("CreateObjectCommand requires a non-empty name");
	if (Parent.IsValid() && !Document.GetScene().FindObject(Parent).IsValid())
		throw std::out_of_range("CreateObjectCommand parent does not exist");
}

string_view CreateObjectCommand::GetName() const noexcept
{
	return "Create Object";
}

void CreateObjectCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("CreateObjectCommand target is already present");
	const world::ObjectHandle ParentHandle =
		this->Parent.IsValid() ? this->Document->GetScene().FindObject(this->Parent) : world::ObjectHandle{};
	if (this->Parent.IsValid() && !ParentHandle.IsValid())
		throw std::out_of_range("CreateObjectCommand parent no longer exists");
	(void)this->Document->CreateObject(this->Name, ParentHandle, this->PersistentID);
	this->Present = true;
}

void CreateObjectCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("CreateObjectCommand target is not present");
	this->Document->DestroyObject(this->PersistentID);
	SetSelection(*this->Document, this->PreviousSelection);
	this->Present = false;
}

const util::UUID &CreateObjectCommand::GetPersistentID() const noexcept
{
	return this->PersistentID;
}

CreateMeshObjectCommand::CreateMeshObjectCommand(document::SceneDocument &Document, string Name,
												 resource::AssetHandle<resource::ModelAsset> Model, const util::UUID Parent)
	: Document(&Document), Name(std::move(Name)), Model(std::move(Model)), Parent(Parent),
	  PreviousSelection(Document.GetSelection().GetOrdered())
{
	if (this->Name.empty())
		throw std::invalid_argument("CreateMeshObjectCommand requires a non-empty name");
	if (!this->Model)
		throw std::invalid_argument("CreateMeshObjectCommand requires a valid model asset");
	if (Parent.IsValid() && !Document.GetScene().FindObject(Parent).IsValid())
		throw std::out_of_range("CreateMeshObjectCommand parent does not exist");
}

string_view CreateMeshObjectCommand::GetName() const noexcept
{
	return "Create Mesh Object";
}

void CreateMeshObjectCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("CreateMeshObjectCommand target is already present");
	const world::ObjectHandle ParentHandle =
		this->Parent.IsValid() ? this->Document->GetScene().FindObject(this->Parent) : world::ObjectHandle{};
	if (this->Parent.IsValid() && !ParentHandle.IsValid())
		throw std::out_of_range("CreateMeshObjectCommand parent no longer exists");
	const world::ObjectHandle Object = this->Document->CreateObject(this->Name, ParentHandle, this->PersistentID);
	try
	{
		(void)this->Document->GetScene().AddComponent<components::CObjectMeshComponent>(Object, this->Model);
		this->Present = true;
	}
	catch (...)
	{
		this->Document->DestroyObject(this->PersistentID);
		SetSelection(*this->Document, this->PreviousSelection);
		throw;
	}
}

void CreateMeshObjectCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("CreateMeshObjectCommand target is not present");
	this->Document->DestroyObject(this->PersistentID);
	SetSelection(*this->Document, this->PreviousSelection);
	this->Present = false;
}

const util::UUID &CreateMeshObjectCommand::GetPersistentID() const noexcept
{
	return this->PersistentID;
}

RenameObjectCommand::RenameObjectCommand(document::SceneDocument &Document, const util::UUID Object, string Name)
	: Document(&Document), Object(Object), After(std::move(Name))
{
	if (this->After.empty())
		throw std::invalid_argument("Object name cannot be empty");
	const world::ObjectHandle Handle = Document.GetScene().FindObject(Object);
	if (!Handle.IsValid())
		throw std::out_of_range("RenameObjectCommand target does not exist");
	auto Access = Document.GetScene().Read();
	this->Before = Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(Handle)).GetName();
}

string_view RenameObjectCommand::GetName() const noexcept
{
	return "Rename Object";
}

void RenameObjectCommand::Execute()
{
	this->Apply(this->After);
}

void RenameObjectCommand::Undo()
{
	this->Apply(this->Before);
}

bool RenameObjectCommand::TryMerge(const EditorCommand &Other)
{
	const auto *Typed = dynamic_cast<const RenameObjectCommand *>(&Other);
	if (Typed == nullptr || Typed->Document != this->Document || Typed->Object != this->Object)
		return false;
	this->After = Typed->After;
	return true;
}

void RenameObjectCommand::Apply(const string_view Value)
{
	const world::ObjectHandle Handle = this->Document->GetScene().FindObject(this->Object);
	if (!Handle.IsValid())
		throw std::out_of_range("RenameObjectCommand target no longer exists");
	auto Access = this->Document->GetScene().Write();
	Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(Handle)).SetName(string(Value));
}

ReparentObjectCommand::ReparentObjectCommand(document::SceneDocument &Document, const util::UUID Object, const util::UUID Parent,
											 const uint32 SiblingOrder, const ReparentTransformRule TransformRule)
	: Document(&Document), Object(Object), AfterParent(Parent), AfterSiblingOrder(SiblingOrder)
{
	const world::ObjectHandle Handle = Document.GetScene().FindObject(Object);
	if (!Handle.IsValid())
		throw std::out_of_range("ReparentObjectCommand target does not exist");
	const world::ObjectHandle ParentHandle = Parent.IsValid() ? Document.GetScene().FindObject(Parent) : world::ObjectHandle{};
	if (Parent.IsValid() && !ParentHandle.IsValid())
		throw std::out_of_range("ReparentObjectCommand parent does not exist");
	const hierarchy::SceneHierarchySnapshot Hierarchy =
		hierarchy::SceneHierarchyBuilder::Build(Document.GetScene(), Document.GetRevision());
	auto Access = Document.GetScene().Read();
	const components::CObjectHierarchyComponent &HierarchyComponent =
		Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(Handle));
	const components::CObjectTransformComponent &Transform =
		Access.Resolve(Access.GetComponent<components::CObjectTransformComponent>(Handle));
	this->BeforeSiblingOrder = HierarchyComponent.GetSiblingOrder();
	this->BeforePosition = Transform.GetPosition();
	this->BeforeRotation = Transform.GetRotation();
	this->BeforeScale = Transform.GetScale();
	this->AfterPosition = this->BeforePosition;
	this->AfterRotation = this->BeforeRotation;
	this->AfterScale = this->BeforeScale;
	if (HierarchyComponent.GetParent().IsValid())
		this->BeforeParent = GetPersistentID(Access, HierarchyComponent.GetParent());

	const auto ParentID = [&Hierarchy](const hierarchy::SceneHierarchyRow &Row)
	{ return Row.ParentRow == hierarchy::InvalidHierarchyRow ? util::UUID{} : Hierarchy.Rows[Row.ParentRow].PersistentID; };
	std::vector<const hierarchy::SceneHierarchyRow *> DestinationSiblings;
	std::vector<const hierarchy::SceneHierarchyRow *> SourceSiblings;
	for (const hierarchy::SceneHierarchyRow &Row : Hierarchy.Rows)
	{
		if (Row.PersistentID == Object)
			continue;
		const util::UUID RowParent = ParentID(Row);
		if (RowParent == this->AfterParent)
			DestinationSiblings.push_back(&Row);
		if (this->BeforeParent != this->AfterParent && RowParent == this->BeforeParent)
			SourceSiblings.push_back(&Row);
	}
	const uint32 InsertionIndex = std::min(this->AfterSiblingOrder, static_cast<uint32>(DestinationSiblings.size()));
	DestinationSiblings.insert(DestinationSiblings.begin() + InsertionIndex, nullptr);
	this->AfterSiblingOrder = InsertionIndex;
	for (uint32 Index = 0; Index < DestinationSiblings.size(); ++Index)
	{
		const hierarchy::SceneHierarchyRow *Row = DestinationSiblings[Index];
		if (Row != nullptr && Row->SiblingOrder != Index)
			this->SiblingOrderEdits.push_back(
				{.Object = Row->PersistentID, .Parent = this->AfterParent, .Before = Row->SiblingOrder, .After = Index});
	}
	for (uint32 Index = 0; Index < SourceSiblings.size(); ++Index)
	{
		const hierarchy::SceneHierarchyRow &Row = *SourceSiblings[Index];
		if (Row.SiblingOrder != Index)
			this->SiblingOrderEdits.push_back(
				{.Object = Row.PersistentID, .Parent = this->BeforeParent, .Before = Row.SiblingOrder, .After = Index});
	}
	if (TransformRule == ReparentTransformRule::PreserveWorld)
	{
		const world::SceneTransformSnapshot Transforms = world::SceneTransformSnapshot::Build(Access);
		const glm::mat4 ParentWorld = Parent.IsValid() ? Transforms.GetMatrix(ParentHandle) : glm::mat4(1.0f);
		const float32 ParentDeterminant = glm::determinant(ParentWorld);
		if (!world::IsFiniteTransformValue(ParentWorld) || !std::isfinite(ParentDeterminant) ||
			std::abs(ParentDeterminant) <= std::numeric_limits<float32>::epsilon())
			throw std::invalid_argument("ReparentObjectCommand cannot preserve world transform through a singular parent");
		const world::DecomposedTransform Local = world::DecomposeAffineTransform(glm::inverse(ParentWorld) * Transforms.GetMatrix(Handle));
		this->AfterPosition = Local.Position;
		this->AfterRotation = Local.Rotation;
		this->AfterScale = Local.Scale;
	}
}

string_view ReparentObjectCommand::GetName() const noexcept
{
	return "Reparent Object";
}

void ReparentObjectCommand::Execute()
{
	usize AppliedEdits = 0;
	bool ParentApplied = false;
	try
	{
		this->Document->SetParent(this->Object, this->AfterParent, this->AfterSiblingOrder);
		ParentApplied = true;
		for (; AppliedEdits < this->SiblingOrderEdits.size(); ++AppliedEdits)
		{
			const SiblingOrderEdit &Edit = this->SiblingOrderEdits[AppliedEdits];
			this->Document->SetParent(Edit.Object, Edit.Parent, Edit.After);
		}
		const world::ObjectHandle Handle = this->Document->GetScene().FindObject(this->Object);
		const auto Transform = this->Document->GetScene().GetComponent<components::CObjectTransformComponent>(Handle);
		auto Access = this->Document->GetScene().Write();
		Access.Resolve(Transform).SetTransform(this->AfterPosition, this->AfterRotation, this->AfterScale);
	}
	catch (...)
	{
		while (AppliedEdits != 0)
		{
			const SiblingOrderEdit &Edit = this->SiblingOrderEdits[--AppliedEdits];
			this->Document->SetParent(Edit.Object, Edit.Parent, Edit.Before);
		}
		if (ParentApplied)
			this->Document->SetParent(this->Object, this->BeforeParent, this->BeforeSiblingOrder);
		throw;
	}
}

void ReparentObjectCommand::Undo()
{
	usize AppliedEdits = 0;
	bool ParentApplied = false;
	try
	{
		this->Document->SetParent(this->Object, this->BeforeParent, this->BeforeSiblingOrder);
		ParentApplied = true;
		for (; AppliedEdits < this->SiblingOrderEdits.size(); ++AppliedEdits)
		{
			const SiblingOrderEdit &Edit = this->SiblingOrderEdits[AppliedEdits];
			this->Document->SetParent(Edit.Object, Edit.Parent, Edit.Before);
		}
		const world::ObjectHandle Handle = this->Document->GetScene().FindObject(this->Object);
		const auto Transform = this->Document->GetScene().GetComponent<components::CObjectTransformComponent>(Handle);
		auto Access = this->Document->GetScene().Write();
		Access.Resolve(Transform).SetTransform(this->BeforePosition, this->BeforeRotation, this->BeforeScale);
	}
	catch (...)
	{
		while (AppliedEdits != 0)
		{
			const SiblingOrderEdit &Edit = this->SiblingOrderEdits[--AppliedEdits];
			this->Document->SetParent(Edit.Object, Edit.Parent, Edit.After);
		}
		if (ParentApplied)
			this->Document->SetParent(this->Object, this->AfterParent, this->AfterSiblingOrder);
		throw;
	}
}

AddComponentCommand::AddComponentCommand(document::SceneDocument &Document, const util::UUID Object, const uint32 ComponentType)
	: Document(&Document), Object(Object), ComponentType(ComponentType)
{
	const world::ObjectHandle Handle = Document.GetScene().FindObject(Object);
	if (!Handle.IsValid())
		throw std::out_of_range("AddComponentCommand target does not exist");
	if (HasEditableComponent(Document.GetScene(), Handle, ComponentType))
		throw world::ComponentAlreadyAttachedException(Handle, "editor component");
}

string_view AddComponentCommand::GetName() const noexcept
{
	return "Add Component";
}

void AddComponentCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("AddComponentCommand component is already present");
	const world::ObjectHandle Handle = this->Document->GetScene().FindObject(this->Object);
	if (!Handle.IsValid())
		throw std::out_of_range("AddComponentCommand target no longer exists");
	AddDefaultComponent(this->Document->GetScene(), Handle, this->ComponentType);
	this->Present = true;
}

void AddComponentCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("AddComponentCommand component is not present");
	const world::ObjectHandle Handle = this->Document->GetScene().FindObject(this->Object);
	if (!Handle.IsValid())
		throw std::out_of_range("AddComponentCommand target no longer exists");
	RemoveComponent(this->Document->GetScene(), Handle, this->ComponentType);
	this->Present = false;
}

class RemoveComponentCommand::State final
{
  public:
	explicit State(ComponentArchiveState Value) : Value(std::move(Value))
	{
	}

	ComponentArchiveState Value;
};

RemoveComponentCommand::RemoveComponentCommand(document::SceneDocument &Document, const util::UUID Object, const uint32 ComponentType)
	: Document(&Document), Object(Object), ComponentType(ComponentType)
{
	const world::ObjectHandle Handle = Document.GetScene().FindObject(Object);
	if (!Handle.IsValid())
		throw std::out_of_range("RemoveComponentCommand target does not exist");
	if (!HasEditableComponent(Document.GetScene(), Handle, ComponentType))
		throw std::out_of_range("RemoveComponentCommand component is not attached");
	this->Before = std::make_unique<State>(CaptureComponent(Document.GetScene(), Handle, ComponentType));
}

RemoveComponentCommand::~RemoveComponentCommand() = default;

string_view RemoveComponentCommand::GetName() const noexcept
{
	return "Remove Component";
}

void RemoveComponentCommand::Execute()
{
	if (!this->Present)
		throw std::logic_error("RemoveComponentCommand component is already absent");
	const world::ObjectHandle Handle = this->Document->GetScene().FindObject(this->Object);
	if (!Handle.IsValid())
		throw std::out_of_range("RemoveComponentCommand target no longer exists");
	RemoveComponent(this->Document->GetScene(), Handle, this->ComponentType);
	this->Present = false;
}

void RemoveComponentCommand::Undo()
{
	if (this->Present)
		throw std::logic_error("RemoveComponentCommand component is already present");
	const world::ObjectHandle Handle = this->Document->GetScene().FindObject(this->Object);
	if (!Handle.IsValid())
		throw std::out_of_range("RemoveComponentCommand target no longer exists");
	try
	{
		RestoreComponent(this->Document->GetScene(), Handle, this->Before->Value);
	}
	catch (...)
	{
		if (HasEditableComponent(this->Document->GetScene(), Handle, this->ComponentType))
			RemoveComponent(this->Document->GetScene(), Handle, this->ComponentType);
		throw;
	}
	this->Present = true;
}

class DeleteObjectsCommand::Archive final
{
  public:
	Archive(document::SceneDocument &Document, const std::vector<util::UUID> &Objects) : Value(Document, Objects, false)
	{
	}

	SceneObjectArchive Value;
};

DeleteObjectsCommand::DeleteObjectsCommand(document::SceneDocument &Document, std::vector<util::UUID> Objects,
										   FinalizationCallback FinalizeDeletedAssets)
	: Document(&Document), State(std::make_unique<Archive>(Document, Objects)), PreviousSelection(Document.GetSelection().GetOrdered()),
	  FinalizeDeletedAssets(std::move(FinalizeDeletedAssets))
{
}

DeleteObjectsCommand::~DeleteObjectsCommand() = default;

string_view DeleteObjectsCommand::GetName() const noexcept
{
	return "Delete Objects";
}

void DeleteObjectsCommand::Execute()
{
	if (!this->Present)
		throw std::logic_error("DeleteObjectsCommand targets are already absent");
	this->State->Value.Destroy(*this->Document);
	this->Present = false;
}

void DeleteObjectsCommand::Undo()
{
	if (this->Present)
		throw std::logic_error("DeleteObjectsCommand targets are already present");
	this->State->Value.Restore(*this->Document);
	SetSelection(*this->Document, this->PreviousSelection);
	this->Present = true;
}

void DeleteObjectsCommand::Finalize()
{
	if (this->Finalized || this->Present || !this->FinalizeDeletedAssets)
		return;
	std::vector<resource::AssetID> MaterialAssets = this->State->Value.GetMaterialOverrideAssetIDs();
	this->FinalizeDeletedAssets(std::move(MaterialAssets));
	this->Finalized = true;
	this->State.reset();
}

class DuplicateObjectsCommand::Archive final
{
  public:
	Archive(document::SceneDocument &Document, const std::vector<util::UUID> &Objects) : Value(Document, Objects, true)
	{
	}

	SceneObjectArchive Value;
};

DuplicateObjectsCommand::DuplicateObjectsCommand(document::SceneDocument &Document, std::vector<util::UUID> Objects)
	: Document(&Document), State(std::make_unique<Archive>(Document, Objects)), PreviousSelection(Document.GetSelection().GetOrdered())
{
}

DuplicateObjectsCommand::~DuplicateObjectsCommand() = default;

string_view DuplicateObjectsCommand::GetName() const noexcept
{
	return "Duplicate Objects";
}

void DuplicateObjectsCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("DuplicateObjectsCommand targets are already present");
	this->State->Value.Restore(*this->Document);
	SetSelection(*this->Document, this->State->Value.GetIDs());
	this->Present = true;
}

void DuplicateObjectsCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("DuplicateObjectsCommand targets are not present");
	this->State->Value.Destroy(*this->Document);
	SetSelection(*this->Document, this->PreviousSelection);
	this->Present = false;
}

const std::vector<util::UUID> &DuplicateObjectsCommand::GetCreatedObjects() const noexcept
{
	return this->State->Value.GetIDs();
}

class PasteObjectsCommand::Archive final
{
  public:
	explicit Archive(const SceneObjectSnapshot &Snapshot) : Value(Snapshot.State->Value.CreateDuplicate())
	{
	}

	SceneObjectArchive Value;
};

PasteObjectsCommand::PasteObjectsCommand(document::SceneDocument &Document, const SceneObjectSnapshot &Snapshot)
	: Document(&Document), PreviousSelection(Document.GetSelection().GetOrdered())
{
	if (Snapshot.Empty())
		throw std::invalid_argument("PasteObjectsCommand requires a non-empty scene-object snapshot");
	this->State = std::make_unique<Archive>(Snapshot);
}

PasteObjectsCommand::~PasteObjectsCommand() = default;

string_view PasteObjectsCommand::GetName() const noexcept
{
	return "Paste Objects";
}

void PasteObjectsCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("PasteObjectsCommand targets are already present");
	this->State->Value.Restore(*this->Document);
	SetSelection(*this->Document, this->State->Value.GetIDs());
	this->Present = true;
}

void PasteObjectsCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("PasteObjectsCommand targets are not present");
	this->State->Value.Destroy(*this->Document);
	SetSelection(*this->Document, this->PreviousSelection);
	this->Present = false;
}

const std::vector<util::UUID> &PasteObjectsCommand::GetCreatedObjects() const noexcept
{
	return this->State->Value.GetIDs();
}
} // namespace editor::commands
