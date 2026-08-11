#include "SceneCloner.h"

#include "src/component/object/CObjectAnimationComponent.h"
#include "src/component/object/CObjectBehaviorComponent.h"
#include "src/component/object/CObjectCameraComponent.h"
#include "src/component/object/CObjectHierarchyComponent.h"
#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectLightComponents.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace world
{
namespace
{
static_assert(components::CObjectComponents == 10U, "SceneCloner must be updated whenever a concrete component type is registered");

struct CloneObjectHandleHash final
{
	[[nodiscard]] usize operator()(const ObjectHandle &Handle) const noexcept
	{
		uint64 Value = Handle.Scene;
		Value ^= static_cast<uint64>(Handle.Slot) << 1U;
		Value ^= static_cast<uint64>(Handle.Generation) << 33U;
		Value ^= Value >> 30U;
		Value *= 0xbf58476d1ce4e5b9ULL;
		Value ^= Value >> 27U;
		Value *= 0x94d049bb133111ebULL;
		Value ^= Value >> 31U;
		return static_cast<usize>(Value);
	}
};

template <IsCObjectComponent ComponentType>
void CopyEnabled(const ComponentType &Source, Scene &DestinationScene, const ComponentHandle<ComponentType> Destination)
{
	auto Access = DestinationScene.Write();
	Access.Resolve(Destination).SetEnabled(Source.IsEnabled());
}

template <IsCObjectComponent LightType> void CopyLightBase(const LightType &Source, LightType &Destination)
{
	Destination.SetColor(Source.GetColor());
	Destination.GetShadowSettings() = Source.GetShadowSettings();
}

struct PendingParent final
{
	ObjectHandle DestinationChild;
	ObjectHandle SourceParent;
	uint32 SiblingOrder = 0;
};
} // namespace

ObjectHandle SceneCloneResult::FindObject(const util::UUID &PersistentID) const noexcept
{
	const auto Object = this->ObjectsByPersistentID.find(PersistentID);
	return Object == this->ObjectsByPersistentID.end() ? ObjectHandle{} : Object->second;
}

SceneCloneResult SceneCloner::Clone(const Scene &Source, const SceneCapacitySpecification Capacity)
{
	SceneCloneResult Result{.ClonedScene = std::make_unique<Scene>(Capacity)};
	std::unordered_map<ObjectHandle, ObjectHandle, CloneObjectHandleHash> ObjectMap;
	std::vector<PendingParent> PendingParents;

	const Scene::ReadAccess SourceAccess = Source.Read();
	const std::vector<ObjectHandle> SourceObjects = SourceAccess.Objects();
	if (SourceObjects.size() > Capacity.Objects)
		throw SceneCapacityException("Scene clone object count exceeds the destination capacity");
	ObjectMap.reserve(SourceObjects.size());
	Result.ObjectsByPersistentID.reserve(SourceObjects.size());
	PendingParents.reserve(SourceObjects.size());

	for (const ObjectHandle SourceObject : SourceObjects)
		ObjectMap.emplace(SourceObject, Result.ClonedScene->CreateObject());

	for (const ObjectHandle SourceObject : SourceObjects)
	{
		const ObjectHandle DestinationObject = ObjectMap.at(SourceObject);

		const ComponentHandle<components::CObjectIdentityComponent> SourceIdentity =
			SourceAccess.GetComponent<components::CObjectIdentityComponent>(SourceObject);
		if (SourceIdentity.IsValid())
		{
			const components::CObjectIdentityComponent &Identity = SourceAccess.Resolve(SourceIdentity);
			const ComponentHandle<components::CObjectIdentityComponent> DestinationIdentity =
				Result.ClonedScene->AddComponent<components::CObjectIdentityComponent>(DestinationObject, Identity.GetName(),
																					   Identity.GetPersistentID());
			{
				auto Access = Result.ClonedScene->Write();
				components::CObjectIdentityComponent &Clone = Access.Resolve(DestinationIdentity);
				Clone.SetTags(std::vector<string>(Identity.GetTags().begin(), Identity.GetTags().end()));
				Clone.SetMobility(Identity.GetMobility());
				Clone.SetEditorVisible(Identity.IsEditorVisible());
				Clone.SetLocked(Identity.IsLocked());
			}
			CopyEnabled(Identity, *Result.ClonedScene, DestinationIdentity);
			if (!Result.ObjectsByPersistentID.emplace(Identity.GetPersistentID(), DestinationObject).second)
				throw SceneException("Scene clone encountered duplicate persistent object identities");
		}

		const ComponentHandle<components::CObjectTransformComponent> SourceTransform =
			SourceAccess.GetComponent<components::CObjectTransformComponent>(SourceObject);
		if (SourceTransform.IsValid())
		{
			const components::CObjectTransformComponent &Transform = SourceAccess.Resolve(SourceTransform);
			const ComponentHandle<components::CObjectTransformComponent> DestinationTransform =
				Result.ClonedScene->AddComponent<components::CObjectTransformComponent>(DestinationObject, Transform.GetPosition(),
																						Transform.GetRotation(), Transform.GetScale());
			CopyEnabled(Transform, *Result.ClonedScene, DestinationTransform);
		}

		const ComponentHandle<components::CObjectHierarchyComponent> SourceHierarchy =
			SourceAccess.GetComponent<components::CObjectHierarchyComponent>(SourceObject);
		if (SourceHierarchy.IsValid())
		{
			const components::CObjectHierarchyComponent &Hierarchy = SourceAccess.Resolve(SourceHierarchy);
			const ComponentHandle<components::CObjectHierarchyComponent> DestinationHierarchy =
				Result.ClonedScene->AddComponent<components::CObjectHierarchyComponent>(DestinationObject);
			CopyEnabled(Hierarchy, *Result.ClonedScene, DestinationHierarchy);
			PendingParents.push_back({.DestinationChild = DestinationObject,
									  .SourceParent = Hierarchy.GetParent(),
									  .SiblingOrder = Hierarchy.GetSiblingOrder()});
		}

		const ComponentHandle<components::CObjectCameraComponent> SourceCamera =
			SourceAccess.GetComponent<components::CObjectCameraComponent>(SourceObject);
		if (SourceCamera.IsValid())
		{
			const components::CObjectCameraComponent &Camera = SourceAccess.Resolve(SourceCamera);
			const ComponentHandle<components::CObjectCameraComponent> DestinationCamera =
				Result.ClonedScene->AddComponent<components::CObjectCameraComponent>(DestinationObject);
			{
				auto Access = Result.ClonedScene->Write();
				components::CObjectCameraComponent &Clone = Access.Resolve(DestinationCamera);
				Clone.SetProjection(Camera.GetProjection());
				Clone.SetVerticalFieldOfViewDegrees(Camera.GetVerticalFieldOfViewDegrees());
				Clone.SetOrthographicHeight(Camera.GetOrthographicHeight());
				Clone.SetClipPlanes(Camera.GetNearPlane(), Camera.GetFarPlane());
				Clone.SetExposureCompensation(Camera.GetExposureCompensation());
				Clone.SetPrimary(Camera.IsPrimary());
				Clone.SetTemporalJitterEnabled(Camera.IsTemporalJitterEnabled());
			}
			CopyEnabled(Camera, *Result.ClonedScene, DestinationCamera);
		}

		const ComponentHandle<components::CObjectPointLightComponent> SourcePointLight =
			SourceAccess.GetComponent<components::CObjectPointLightComponent>(SourceObject);
		if (SourcePointLight.IsValid())
		{
			const components::CObjectPointLightComponent &Light = SourceAccess.Resolve(SourcePointLight);
			const ComponentHandle<components::CObjectPointLightComponent> DestinationLight =
				Result.ClonedScene->AddComponent<components::CObjectPointLightComponent>(DestinationObject);
			{
				auto Access = Result.ClonedScene->Write();
				components::CObjectPointLightComponent &Clone = Access.Resolve(DestinationLight);
				CopyLightBase(Light, Clone);
				Clone.SetLuminousPowerLumens(Light.GetLuminousPowerLumens());
				Clone.SetRange(Light.GetRange());
				Clone.SetSourceRadius(Light.GetSourceRadius());
			}
			CopyEnabled(Light, *Result.ClonedScene, DestinationLight);
		}

		const ComponentHandle<components::CObjectSpotLightComponent> SourceSpotLight =
			SourceAccess.GetComponent<components::CObjectSpotLightComponent>(SourceObject);
		if (SourceSpotLight.IsValid())
		{
			const components::CObjectSpotLightComponent &Light = SourceAccess.Resolve(SourceSpotLight);
			const ComponentHandle<components::CObjectSpotLightComponent> DestinationLight =
				Result.ClonedScene->AddComponent<components::CObjectSpotLightComponent>(DestinationObject);
			{
				auto Access = Result.ClonedScene->Write();
				components::CObjectSpotLightComponent &Clone = Access.Resolve(DestinationLight);
				CopyLightBase(Light, Clone);
				Clone.SetLuminousPowerLumens(Light.GetLuminousPowerLumens());
				Clone.SetRange(Light.GetRange());
				Clone.SetConeAngles(Light.GetInnerConeDegrees(), Light.GetOuterConeDegrees());
			}
			CopyEnabled(Light, *Result.ClonedScene, DestinationLight);
		}

		const ComponentHandle<components::CObjectDirectionalLightComponent> SourceDirectionalLight =
			SourceAccess.GetComponent<components::CObjectDirectionalLightComponent>(SourceObject);
		if (SourceDirectionalLight.IsValid())
		{
			const components::CObjectDirectionalLightComponent &Light = SourceAccess.Resolve(SourceDirectionalLight);
			const ComponentHandle<components::CObjectDirectionalLightComponent> DestinationLight =
				Result.ClonedScene->AddComponent<components::CObjectDirectionalLightComponent>(DestinationObject);
			{
				auto Access = Result.ClonedScene->Write();
				components::CObjectDirectionalLightComponent &Clone = Access.Resolve(DestinationLight);
				CopyLightBase(Light, Clone);
				Clone.SetIlluminanceLux(Light.GetIlluminanceLux());
				Clone.SetAngularDiameterDegrees(Light.GetAngularDiameterDegrees());
				Clone.SetCascadeCount(Light.GetCascadeCount());
				Clone.SetCascadeDistributionExponent(Light.GetCascadeDistributionExponent());
			}
			CopyEnabled(Light, *Result.ClonedScene, DestinationLight);
		}

		const ComponentHandle<components::CObjectMeshComponent> SourceMesh =
			SourceAccess.GetComponent<components::CObjectMeshComponent>(SourceObject);
		if (SourceMesh.IsValid())
		{
			const components::CObjectMeshComponent &Mesh = SourceAccess.Resolve(SourceMesh);
			const ComponentHandle<components::CObjectMeshComponent> DestinationMesh =
				Result.ClonedScene->AddComponent<components::CObjectMeshComponent>(DestinationObject, Mesh.GetModel());
			{
				auto Access = Result.ClonedScene->Write();
				components::CObjectMeshComponent &Clone = Access.Resolve(DestinationMesh);
				Clone.SetVisibility(Mesh.GetVisibility());
				Clone.SetLODPolicy(Mesh.GetLODPolicy());
				Clone.SetRenderLayerMask(Mesh.GetRenderLayerMask());
				for (const components::MeshMaterialOverride &Override : Mesh.GetMaterialOverrides())
					Clone.SetMaterialOverride(Override.MeshInstance, Override.MaterialSlot, Override.Material);
			}
			CopyEnabled(Mesh, *Result.ClonedScene, DestinationMesh);
		}

		const ComponentHandle<components::CObjectAnimationComponent> SourceAnimation =
			SourceAccess.GetComponent<components::CObjectAnimationComponent>(SourceObject);
		if (SourceAnimation.IsValid())
		{
			const components::CObjectAnimationComponent &Animation = SourceAccess.Resolve(SourceAnimation);
			const ComponentHandle<components::CObjectAnimationComponent> DestinationAnimation =
				Result.ClonedScene->AddComponent<components::CObjectAnimationComponent>(DestinationObject, Animation.GetGraph());
			{
				auto Access = Result.ClonedScene->Write();
				components::CObjectAnimationComponent &Clone = Access.Resolve(DestinationAnimation);
				Clone.SetUpdateMode(Animation.GetUpdateMode());
				Clone.SetRootMotionEnabled(Animation.IsRootMotionEnabled());
				for (const components::AnimationParameterValue &Parameter : Animation.GetParameters())
					Clone.SetParameter(Parameter.ID, Parameter.Type, Parameter.Value);
				for (const components::AnimationMorphWeight &Weight : Animation.GetMorphWeights())
					Clone.SetMorphWeight(Weight.MorphSet, Weight.Target, Weight.Weight);
				for (const resource::AssetHandle<resource::RetargetProfileAsset> &Profile : Animation.GetRetargetProfiles())
					Clone.SetRetargetProfile(Profile);
			}
			CopyEnabled(Animation, *Result.ClonedScene, DestinationAnimation);
		}

		const ComponentHandle<components::CObjectBehaviorComponent> SourceBehavior =
			SourceAccess.GetComponent<components::CObjectBehaviorComponent>(SourceObject);
		if (SourceBehavior.IsValid())
		{
			const components::CObjectBehaviorComponent &Behavior = SourceAccess.Resolve(SourceBehavior);
			const ComponentHandle<components::CObjectBehaviorComponent> DestinationBehavior =
				Result.ClonedScene->AddComponent<components::CObjectBehaviorComponent>(DestinationObject);
			{
				auto Access = Result.ClonedScene->Write();
				components::CObjectBehaviorComponent &Clone = Access.Resolve(DestinationBehavior);
				std::vector<components::BehaviorInstance> Instances = Behavior.GetBehaviors();
				for (components::BehaviorInstance &Instance : Instances)
				{
					Instance.State = components::BehaviorExecutionState::Unresolved;
					Instance.Diagnostic.clear();
				}
				Clone.ReplaceBehaviors(std::move(Instances));
			}
			CopyEnabled(Behavior, *Result.ClonedScene, DestinationBehavior);
		}
	}

	for (const PendingParent &Parent : PendingParents)
	{
		if (!Parent.SourceParent.IsValid())
			continue;
		const auto DestinationParent = ObjectMap.find(Parent.SourceParent);
		if (DestinationParent == ObjectMap.end())
			throw SceneException("Scene clone hierarchy references an object outside the source scene");
		Result.ClonedScene->SetParent(Parent.DestinationChild, DestinationParent->second, Parent.SiblingOrder);
	}
	return Result;
}
} // namespace world
