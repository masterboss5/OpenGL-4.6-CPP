#include "ComponentReflection.h"

#include "Source/component/object/CObjectAnimationComponent.h"
#include "Source/component/object/CObjectBehaviorComponent.h"
#include "Source/component/object/CObjectCameraComponent.h"
#include "Source/component/object/CObjectHierarchyComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectLightComponents.h"
#include "Source/component/object/CObjectMeshComponent.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/resource/asset/AssetManager.h"
#include "Source/scene/Scene.h"

#include <limits>
#include <stdexcept>

namespace editor::reflection
{
namespace
{
template <IsCObjectComponent ComponentType> [[nodiscard]] PropertyDescriptor MakeEnabledProperty()
{
	PropertyDescriptor Descriptor = MakeProperty<ComponentType, bool>(
		"Enabled", "Enabled", "Component", PropertyKind::Boolean, [](const ComponentType &Component) { return Component.IsEnabled(); },
		[](ComponentType &Component, const bool &Value, const PropertyWriteContext &) { Component.SetEnabled(Value); });
	Descriptor.DefaultValue = true;
	return Descriptor;
}

template <IsCObjectComponent ComponentType> [[nodiscard]] TypeDescriptor MakeComponentDescriptor(const string_view DisplayName)
{
	TypeDescriptor Descriptor;
	Descriptor.Name = "components." + string(ComponentType::ComponentName);
	Descriptor.DisplayName = string(DisplayName);
	Descriptor.Properties.push_back(MakeEnabledProperty<ComponentType>());
	return Descriptor;
}

[[nodiscard]] PropertyDescriptor MakeAssetProperty(
	string Name, string DisplayName, string Category, const resource::AssetType Type,
	std::function<resource::AssetID(const components::CObjectMeshComponent &)> Read,
	std::function<void(components::CObjectMeshComponent &, resource::AssetManager &, const std::filesystem::path &)> Write)
{
	return MakeProperty<components::CObjectMeshComponent, AssetReference>(
		std::move(Name), std::move(DisplayName), std::move(Category), PropertyKind::AssetReference,
		[Read = std::move(Read), Type](const components::CObjectMeshComponent &Component)
		{ return AssetReference{.ID = Read(Component), .Type = Type}; },
		[Write = std::move(Write), Type](components::CObjectMeshComponent &Component, const AssetReference &Reference,
										 const PropertyWriteContext &Context)
		{
			if (Reference.Type != Type)
				throw std::invalid_argument("Reflected asset assignment has an incompatible asset type");
			if (Context.Assets == nullptr)
				throw std::logic_error("Reflected asset assignment requires an AssetManager write context");
			const resource::AssetRecordHandle Record = Context.Assets->GetRecord(Reference.ID);
			if (Record == nullptr || Record->GetType() != Type)
				throw std::out_of_range("Reflected asset assignment references an unknown asset record");
			Write(Component, *Context.Assets, Record->GetCanonicalPath());
		});
}

[[nodiscard]] TypeDescriptor MakeIdentityDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectIdentityComponent>("Identity");
	Descriptor.Properties.push_back(MakeReadOnlyProperty<components::CObjectIdentityComponent, util::UUID>(
		"PersistentID", "Persistent ID", "Identity", PropertyKind::UUID,
		[](const components::CObjectIdentityComponent &Component) { return Component.GetPersistentID(); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectIdentityComponent, string>(
		"Name", "Name", "Identity", PropertyKind::String,
		[](const components::CObjectIdentityComponent &Component) { return Component.GetName(); },
		[](components::CObjectIdentityComponent &Component, const string &Value, const PropertyWriteContext &)
		{ Component.SetName(Value); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectIdentityComponent, std::vector<string>>(
		"Tags", "Tags", "Identity", PropertyKind::StringList, [](const components::CObjectIdentityComponent &Component)
		{ return std::vector<string>(Component.GetTags().begin(), Component.GetTags().end()); },
		[](components::CObjectIdentityComponent &Component, const std::vector<string> &Value, const PropertyWriteContext &)
		{ Component.SetTags(Value); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectIdentityComponent, uint32>(
		"Mobility", "Mobility", "Identity", PropertyKind::UnsignedInteger,
		[](const components::CObjectIdentityComponent &Component) { return static_cast<uint32>(Component.GetMobility()); },
		[](components::CObjectIdentityComponent &Component, const uint32 &Value, const PropertyWriteContext &)
		{ Component.SetMobility(static_cast<components::ObjectMobility>(Value)); }, PropertyFlags::None, {},
		{{static_cast<uint32>(components::ObjectMobility::Static), "Static"},
		 {static_cast<uint32>(components::ObjectMobility::Stationary), "Stationary"},
		 {static_cast<uint32>(components::ObjectMobility::Movable), "Movable"}}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectIdentityComponent, bool>(
		"EditorVisible", "Visible in Editor", "Editor", PropertyKind::Boolean,
		[](const components::CObjectIdentityComponent &Component) { return Component.IsEditorVisible(); },
		[](components::CObjectIdentityComponent &Component, const bool &Value, const PropertyWriteContext &)
		{ Component.SetEditorVisible(Value); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectIdentityComponent, bool>(
		"Locked", "Locked", "Editor", PropertyKind::Boolean,
		[](const components::CObjectIdentityComponent &Component) { return Component.IsLocked(); },
		[](components::CObjectIdentityComponent &Component, const bool &Value, const PropertyWriteContext &)
		{ Component.SetLocked(Value); }));
	return Descriptor;
}

[[nodiscard]] TypeDescriptor MakeTransformDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectTransformComponent>("Transform");
	Descriptor.Properties.push_back(MakeProperty<components::CObjectTransformComponent, glm::vec3>(
		"Position", "Position", "Transform", PropertyKind::Vector3,
		[](const components::CObjectTransformComponent &Component) { return Component.GetPosition(); },
		[](components::CObjectTransformComponent &Component, const glm::vec3 &Value, const PropertyWriteContext &)
		{ Component.SetPosition(Value); }, PropertyFlags::None, {.Step = 0.1}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectTransformComponent, glm::vec3>(
		"RotationEuler", "Rotation", "Transform", PropertyKind::Vector3,
		[](const components::CObjectTransformComponent &Component) { return Component.GetRotationEuler(); },
		[](components::CObjectTransformComponent &Component, const glm::vec3 &Value, const PropertyWriteContext &)
		{ Component.SetRotationEuler(Value); }, PropertyFlags::Angle, {.Step = 0.1}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectTransformComponent, glm::vec3>(
		"Scale", "Scale", "Transform", PropertyKind::Vector3,
		[](const components::CObjectTransformComponent &Component) { return Component.GetScale(); },
		[](components::CObjectTransformComponent &Component, const glm::vec3 &Value, const PropertyWriteContext &)
		{ Component.SetScale(Value); }, PropertyFlags::None, {.Step = 0.01}));
	return Descriptor;
}

[[nodiscard]] TypeDescriptor MakeHierarchyDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectHierarchyComponent>("Hierarchy");
	Descriptor.Properties.push_back(MakeReadOnlyProperty<components::CObjectHierarchyComponent, world::ObjectHandle>(
		"Parent", "Parent", "Hierarchy", PropertyKind::ObjectReference,
		[](const components::CObjectHierarchyComponent &Component) { return Component.GetParent(); }));
	Descriptor.Properties.push_back(MakeReadOnlyProperty<components::CObjectHierarchyComponent, uint32>(
		"SiblingOrder", "Sibling Order", "Hierarchy", PropertyKind::UnsignedInteger,
		[](const components::CObjectHierarchyComponent &Component) { return Component.GetSiblingOrder(); }));
	return Descriptor;
}

[[nodiscard]] TypeDescriptor MakeCameraDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectCameraComponent>("Camera");
	Descriptor.Properties.push_back(MakeProperty<components::CObjectCameraComponent, uint32>(
		"Projection", "Projection", "Projection", PropertyKind::UnsignedInteger,
		[](const components::CObjectCameraComponent &Component) { return static_cast<uint32>(Component.GetProjection()); },
		[](components::CObjectCameraComponent &Component, const uint32 &Value, const PropertyWriteContext &)
		{
			if (Value > static_cast<uint32>(components::CameraProjection::Orthographic))
				throw std::out_of_range("Reflected camera projection is invalid");
			Component.SetProjection(static_cast<components::CameraProjection>(Value));
		},
		PropertyFlags::None, {},
		{{static_cast<uint32>(components::CameraProjection::Perspective), "Perspective"},
		 {static_cast<uint32>(components::CameraProjection::Orthographic), "Orthographic"}}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectCameraComponent, float32>(
		"VerticalFieldOfViewDegrees", "Field of View", "Projection", PropertyKind::Scalar,
		[](const components::CObjectCameraComponent &Component) { return Component.GetVerticalFieldOfViewDegrees(); },
		[](components::CObjectCameraComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetVerticalFieldOfViewDegrees(Value); }, PropertyFlags::Angle, {.Minimum = 1.0, .Maximum = 179.0, .Step = 0.1}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectCameraComponent, float32>(
		"OrthographicHeight", "Orthographic Height", "Projection", PropertyKind::Scalar,
		[](const components::CObjectCameraComponent &Component) { return Component.GetOrthographicHeight(); },
		[](components::CObjectCameraComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetOrthographicHeight(Value); }, PropertyFlags::None, {.Minimum = 0.001, .Step = 0.1}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectCameraComponent, float32>(
		"NearPlane", "Near Plane", "Clipping", PropertyKind::Scalar,
		[](const components::CObjectCameraComponent &Component) { return Component.GetNearPlane(); },
		[](components::CObjectCameraComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetClipPlanes(Value, Component.GetFarPlane()); }, PropertyFlags::None, {.Minimum = 0.0001, .Step = 0.01}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectCameraComponent, float32>(
		"FarPlane", "Far Plane", "Clipping", PropertyKind::Scalar,
		[](const components::CObjectCameraComponent &Component) { return Component.GetFarPlane(); },
		[](components::CObjectCameraComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetClipPlanes(Component.GetNearPlane(), Value); }, PropertyFlags::None, {.Minimum = 0.001, .Step = 1.0}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectCameraComponent, float32>(
		"ExposureCompensation", "Exposure Compensation", "Exposure", PropertyKind::Scalar,
		[](const components::CObjectCameraComponent &Component) { return Component.GetExposureCompensation(); },
		[](components::CObjectCameraComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetExposureCompensation(Value); }, PropertyFlags::None, {.Step = 0.1}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectCameraComponent, bool>(
		"Primary", "Primary", "Camera", PropertyKind::Boolean,
		[](const components::CObjectCameraComponent &Component) { return Component.IsPrimary(); },
		[](components::CObjectCameraComponent &Component, const bool &Value, const PropertyWriteContext &)
		{ Component.SetPrimary(Value); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectCameraComponent, bool>(
		"TemporalJitterEnabled", "Temporal Jitter", "Camera", PropertyKind::Boolean,
		[](const components::CObjectCameraComponent &Component) { return Component.IsTemporalJitterEnabled(); },
		[](components::CObjectCameraComponent &Component, const bool &Value, const PropertyWriteContext &)
		{ Component.SetTemporalJitterEnabled(Value); }));
	return Descriptor;
}

[[nodiscard]] TypeDescriptor MakeMeshDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectMeshComponent>("Mesh");
	Descriptor.Properties.push_back(MakeAssetProperty(
		"Model", "Model", "Mesh", resource::AssetType::Model,
		[](const components::CObjectMeshComponent &Component) { return Component.GetModel().GetID(); },
		[](components::CObjectMeshComponent &Component, resource::AssetManager &Assets, const std::filesystem::path &Path)
		{ Component.SetModel(Assets.GetAsset<resource::ModelAsset>(Path)); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectMeshComponent, uint32>(
		"Visibility", "Visibility Flags", "Rendering", PropertyKind::UnsignedInteger,
		[](const components::CObjectMeshComponent &Component) { return static_cast<uint32>(Component.GetVisibility()); },
		[](components::CObjectMeshComponent &Component, const uint32 &Value, const PropertyWriteContext &)
		{ Component.SetVisibility(static_cast<components::MeshVisibilityFlags>(Value)); }, PropertyFlags::Bitmask, {},
		{{static_cast<uint32>(components::MeshVisibilityFlags::Visible), "Visible"},
		 {static_cast<uint32>(components::MeshVisibilityFlags::CastsShadows), "Casts Shadows"},
		 {static_cast<uint32>(components::MeshVisibilityFlags::ReceivesShadows), "Receives Shadows"},
		 {static_cast<uint32>(components::MeshVisibilityFlags::VisibleInReflections), "Visible in Reflections"}},
		PropertyValue(static_cast<uint32>(components::MeshVisibilityFlags::Visible | components::MeshVisibilityFlags::CastsShadows |
										  components::MeshVisibilityFlags::ReceivesShadows |
										  components::MeshVisibilityFlags::VisibleInReflections))));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectMeshComponent, uint32>(
		"LODMode", "LOD Mode", "Level of Detail", PropertyKind::UnsignedInteger,
		[](const components::CObjectMeshComponent &Component) { return static_cast<uint32>(Component.GetLODPolicy().Mode); },
		[](components::CObjectMeshComponent &Component, const uint32 &Value, const PropertyWriteContext &)
		{
			if (Value > static_cast<uint32>(components::MeshLODSelectionMode::Forced))
				throw std::out_of_range("Reflected mesh LOD mode is invalid");
			components::MeshLODPolicy Policy = Component.GetLODPolicy();
			Policy.Mode = static_cast<components::MeshLODSelectionMode>(Value);
			Component.SetLODPolicy(Policy);
		},
		PropertyFlags::None, {},
		{{static_cast<uint32>(components::MeshLODSelectionMode::Automatic), "Automatic"},
		 {static_cast<uint32>(components::MeshLODSelectionMode::Biased), "Biased"},
		 {static_cast<uint32>(components::MeshLODSelectionMode::Forced), "Forced"}},
		PropertyValue(static_cast<uint32>(components::MeshLODSelectionMode::Automatic))));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectMeshComponent, int32>(
		"LODBias", "LOD Bias", "Level of Detail", PropertyKind::SignedInteger,
		[](const components::CObjectMeshComponent &Component) { return Component.GetLODPolicy().Bias; },
		[](components::CObjectMeshComponent &Component, const int32 &Value, const PropertyWriteContext &)
		{
			components::MeshLODPolicy Policy = Component.GetLODPolicy();
			Policy.Bias = Value;
			Component.SetLODPolicy(Policy);
		},
		PropertyFlags::None, {.Step = 1.0}, {}, PropertyValue(int32{0})));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectMeshComponent, uint32>(
		"ForcedLOD", "Forced LOD", "Level of Detail", PropertyKind::UnsignedInteger,
		[](const components::CObjectMeshComponent &Component) { return Component.GetLODPolicy().ForcedLOD; },
		[](components::CObjectMeshComponent &Component, const uint32 &Value, const PropertyWriteContext &)
		{
			components::MeshLODPolicy Policy = Component.GetLODPolicy();
			Policy.ForcedLOD = Value;
			Component.SetLODPolicy(Policy);
		},
		PropertyFlags::None, {.Minimum = 0.0, .Step = 1.0}, {}, PropertyValue(uint32{0})));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectMeshComponent, uint32>(
		"RenderLayerMask", "Render Layer Mask", "Rendering", PropertyKind::UnsignedInteger,
		[](const components::CObjectMeshComponent &Component) { return Component.GetRenderLayerMask(); },
		[](components::CObjectMeshComponent &Component, const uint32 &Value, const PropertyWriteContext &)
		{ Component.SetRenderLayerMask(Value); }, PropertyFlags::None, {}, {}, PropertyValue(~uint32{0})));
	return Descriptor;
}

template <IsCObjectComponent LightType> void AddShadowProperties(TypeDescriptor &Descriptor)
{
	Descriptor.Properties.push_back(MakeProperty<LightType, bool>(
		"CastShadows", "Cast Shadows", "Shadows", PropertyKind::Boolean,
		[](const LightType &Component) { return Component.GetShadowSettings().CastShadows; },
		[](LightType &Component, const bool &Value, const PropertyWriteContext &) { Component.GetShadowSettings().CastShadows = Value; }));
	Descriptor.Properties.push_back(MakeProperty<LightType, uint32>(
		"ShadowResolution", "Resolution", "Shadows", PropertyKind::UnsignedInteger,
		[](const LightType &Component) { return static_cast<uint32>(Component.GetShadowSettings().Resolution); },
		[](LightType &Component, const uint32 &Value, const PropertyWriteContext &)
		{
			switch (Value)
			{
			case 256:
			case 512:
			case 1'024:
			case 2'048:
			case 4'096:
			case 8'192:
				Component.GetShadowSettings().Resolution = static_cast<components::ShadowResolution>(Value);
				break;
			default:
				throw std::out_of_range("Reflected shadow resolution is unsupported");
			}
		},
		PropertyFlags::None, {}, {{256, "256"}, {512, "512"}, {1'024, "1024"}, {2'048, "2048"}, {4'096, "4096"}, {8'192, "8192"}}));
	Descriptor.Properties.push_back(MakeProperty<LightType, float32>(
		"ShadowConstantBias", "Constant Bias", "Shadows", PropertyKind::Scalar, [](const LightType &Component)
		{ return Component.GetShadowSettings().ConstantBias; }, [](LightType &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.GetShadowSettings().ConstantBias = Value; }, PropertyFlags::Advanced, {.Minimum = 0.0, .Step = 0.0001}));
	Descriptor.Properties.push_back(MakeProperty<LightType, float32>(
		"ShadowSlopeBias", "Slope Bias", "Shadows", PropertyKind::Scalar, [](const LightType &Component)
		{ return Component.GetShadowSettings().SlopeBias; }, [](LightType &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.GetShadowSettings().SlopeBias = Value; }, PropertyFlags::Advanced, {.Minimum = 0.0, .Step = 0.01}));
	Descriptor.Properties.push_back(MakeProperty<LightType, float32>(
		"ShadowNormalBias", "Normal Bias", "Shadows", PropertyKind::Scalar, [](const LightType &Component)
		{ return Component.GetShadowSettings().NormalBias; }, [](LightType &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.GetShadowSettings().NormalBias = Value; }, PropertyFlags::Advanced, {.Minimum = 0.0, .Step = 0.001}));
	Descriptor.Properties.push_back(MakeProperty<LightType, float32>(
		"ShadowFilterRadius", "Filter Radius", "Shadows", PropertyKind::Scalar, [](const LightType &Component)
		{ return Component.GetShadowSettings().FilterRadius; }, [](LightType &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.GetShadowSettings().FilterRadius = Value; }, PropertyFlags::None, {.Minimum = 0.0, .Step = 0.1}));
}

[[nodiscard]] TypeDescriptor MakePointLightDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectPointLightComponent>("Point Light");
	Descriptor.Properties.push_back(MakeProperty<components::CObjectPointLightComponent, glm::vec3>(
		"Color", "Color", "Light", PropertyKind::Color,
		[](const components::CObjectPointLightComponent &Component) { return Component.GetColor(); },
		[](components::CObjectPointLightComponent &Component, const glm::vec3 &Value, const PropertyWriteContext &)
		{ Component.SetColor(Value); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectPointLightComponent, float32>(
		"LuminousPowerLumens", "Luminous Power", "Light", PropertyKind::Scalar,
		[](const components::CObjectPointLightComponent &Component) { return Component.GetLuminousPowerLumens(); },
		[](components::CObjectPointLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetLuminousPowerLumens(Value); }, PropertyFlags::None, {.Minimum = 0.0, .Step = 10.0}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectPointLightComponent, float32>(
		"Range", "Range", "Light", PropertyKind::Scalar,
		[](const components::CObjectPointLightComponent &Component) { return Component.GetRange(); },
		[](components::CObjectPointLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetRange(Value); }, PropertyFlags::None, {.Minimum = 0.001, .Step = 0.1}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectPointLightComponent, float32>(
		"SourceRadius", "Source Radius", "Light", PropertyKind::Scalar,
		[](const components::CObjectPointLightComponent &Component) { return Component.GetSourceRadius(); },
		[](components::CObjectPointLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetSourceRadius(Value); }, PropertyFlags::None, {.Minimum = 0.0, .Step = 0.01}));
	AddShadowProperties<components::CObjectPointLightComponent>(Descriptor);
	return Descriptor;
}

[[nodiscard]] TypeDescriptor MakeSpotLightDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectSpotLightComponent>("Spot Light");
	Descriptor.Properties.push_back(MakeProperty<components::CObjectSpotLightComponent, glm::vec3>(
		"Color", "Color", "Light", PropertyKind::Color,
		[](const components::CObjectSpotLightComponent &Component) { return Component.GetColor(); },
		[](components::CObjectSpotLightComponent &Component, const glm::vec3 &Value, const PropertyWriteContext &)
		{ Component.SetColor(Value); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectSpotLightComponent, float32>(
		"LuminousPowerLumens", "Luminous Power", "Light", PropertyKind::Scalar,
		[](const components::CObjectSpotLightComponent &Component) { return Component.GetLuminousPowerLumens(); },
		[](components::CObjectSpotLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetLuminousPowerLumens(Value); }, PropertyFlags::None, {.Minimum = 0.0, .Step = 10.0}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectSpotLightComponent, float32>(
		"Range", "Range", "Light", PropertyKind::Scalar,
		[](const components::CObjectSpotLightComponent &Component) { return Component.GetRange(); },
		[](components::CObjectSpotLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetRange(Value); }, PropertyFlags::None, {.Minimum = 0.001, .Step = 0.1}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectSpotLightComponent, float32>(
		"InnerConeDegrees", "Inner Cone", "Light", PropertyKind::Scalar,
		[](const components::CObjectSpotLightComponent &Component) { return Component.GetInnerConeDegrees(); },
		[](components::CObjectSpotLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetConeAngles(Value, Component.GetOuterConeDegrees()); }, PropertyFlags::Angle,
		{.Minimum = 0.0, .Maximum = 179.0, .Step = 0.1}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectSpotLightComponent, float32>(
		"OuterConeDegrees", "Outer Cone", "Light", PropertyKind::Scalar,
		[](const components::CObjectSpotLightComponent &Component) { return Component.GetOuterConeDegrees(); },
		[](components::CObjectSpotLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetConeAngles(Component.GetInnerConeDegrees(), Value); }, PropertyFlags::Angle,
		{.Minimum = 0.0, .Maximum = 179.0, .Step = 0.1}));
	AddShadowProperties<components::CObjectSpotLightComponent>(Descriptor);
	return Descriptor;
}

[[nodiscard]] TypeDescriptor MakeDirectionalLightDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectDirectionalLightComponent>("Directional Light");
	Descriptor.Properties.push_back(MakeProperty<components::CObjectDirectionalLightComponent, glm::vec3>(
		"Color", "Color", "Light", PropertyKind::Color,
		[](const components::CObjectDirectionalLightComponent &Component) { return Component.GetColor(); },
		[](components::CObjectDirectionalLightComponent &Component, const glm::vec3 &Value, const PropertyWriteContext &)
		{ Component.SetColor(Value); }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectDirectionalLightComponent, float32>(
		"IlluminanceLux", "Illuminance", "Light", PropertyKind::Scalar,
		[](const components::CObjectDirectionalLightComponent &Component) { return Component.GetIlluminanceLux(); },
		[](components::CObjectDirectionalLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetIlluminanceLux(Value); }, PropertyFlags::None, {.Minimum = 0.0, .Step = 100.0}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectDirectionalLightComponent, float32>(
		"AngularDiameterDegrees", "Angular Diameter", "Light", PropertyKind::Scalar,
		[](const components::CObjectDirectionalLightComponent &Component) { return Component.GetAngularDiameterDegrees(); },
		[](components::CObjectDirectionalLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetAngularDiameterDegrees(Value); }, PropertyFlags::Angle, {.Minimum = 0.0, .Maximum = 180.0, .Step = 0.01}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectDirectionalLightComponent, uint32>(
		"CascadeCount", "Cascade Count", "Cascades", PropertyKind::UnsignedInteger,
		[](const components::CObjectDirectionalLightComponent &Component) { return Component.GetCascadeCount(); },
		[](components::CObjectDirectionalLightComponent &Component, const uint32 &Value, const PropertyWriteContext &)
		{ Component.SetCascadeCount(Value); }, PropertyFlags::None, {.Minimum = 1.0, .Maximum = 8.0, .Step = 1.0}));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectDirectionalLightComponent, float32>(
		"CascadeDistributionExponent", "Distribution Exponent", "Cascades", PropertyKind::Scalar,
		[](const components::CObjectDirectionalLightComponent &Component) { return Component.GetCascadeDistributionExponent(); },
		[](components::CObjectDirectionalLightComponent &Component, const float32 &Value, const PropertyWriteContext &)
		{ Component.SetCascadeDistributionExponent(Value); }, PropertyFlags::None, {.Minimum = 0.01, .Step = 0.1}));
	AddShadowProperties<components::CObjectDirectionalLightComponent>(Descriptor);
	return Descriptor;
}

[[nodiscard]] TypeDescriptor MakeAnimationDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectAnimationComponent>("Animation");
	Descriptor.Properties.push_back(MakeReadOnlyProperty<components::CObjectAnimationComponent, AssetReference>(
		"Graph", "Animation Graph", "Animation", PropertyKind::AssetReference, [](const components::CObjectAnimationComponent &Component)
		{ return AssetReference{.ID = Component.GetGraph().GetID(), .Type = resource::AssetType::AnimationGraph}; }));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectAnimationComponent, uint32>(
		"UpdateMode", "Update Mode", "Animation", PropertyKind::UnsignedInteger,
		[](const components::CObjectAnimationComponent &Component) { return static_cast<uint32>(Component.GetUpdateMode()); },
		[](components::CObjectAnimationComponent &Component, const uint32 &Value, const PropertyWriteContext &)
		{
			if (Value > static_cast<uint32>(components::AnimationUpdateMode::FixedRate))
				throw std::out_of_range("Reflected animation update mode is invalid");
			Component.SetUpdateMode(static_cast<components::AnimationUpdateMode>(Value));
		},
		PropertyFlags::None, {},
		{{static_cast<uint32>(components::AnimationUpdateMode::Always), "Always"},
		 {static_cast<uint32>(components::AnimationUpdateMode::VisibleOnly), "Visible Only"},
		 {static_cast<uint32>(components::AnimationUpdateMode::FixedRate), "Fixed Rate"}},
		PropertyValue(static_cast<uint32>(components::AnimationUpdateMode::Always))));
	Descriptor.Properties.push_back(MakeProperty<components::CObjectAnimationComponent, bool>(
		"RootMotionEnabled", "Root Motion", "Animation", PropertyKind::Boolean,
		[](const components::CObjectAnimationComponent &Component) { return Component.IsRootMotionEnabled(); },
		[](components::CObjectAnimationComponent &Component, const bool &Value, const PropertyWriteContext &)
		{ Component.SetRootMotionEnabled(Value); }, PropertyFlags::None, {}, {}, PropertyValue(false)));
	Descriptor.Properties.push_back(MakeReadOnlyProperty<components::CObjectAnimationComponent, uint32>(
		"ParameterCount", "Parameters", "Runtime", PropertyKind::UnsignedInteger, [](const components::CObjectAnimationComponent &Component)
		{ return static_cast<uint32>(Component.GetParameters().size()); }, PropertyFlags::Advanced));
	Descriptor.Properties.push_back(MakeReadOnlyProperty<components::CObjectAnimationComponent, uint32>(
		"MorphWeightCount", "Morph Weights", "Runtime", PropertyKind::UnsignedInteger,
		[](const components::CObjectAnimationComponent &Component) { return static_cast<uint32>(Component.GetMorphWeights().size()); },
		PropertyFlags::Advanced));
	return Descriptor;
}

[[nodiscard]] TypeDescriptor MakeBehaviorDescriptor()
{
	TypeDescriptor Descriptor = MakeComponentDescriptor<components::CObjectBehaviorComponent>("Behaviors");
	Descriptor.Properties.push_back(MakeReadOnlyProperty<components::CObjectBehaviorComponent, uint32>(
		"BehaviorCount", "Behavior Count", "Behaviors", PropertyKind::UnsignedInteger,
		[](const components::CObjectBehaviorComponent &Component) { return static_cast<uint32>(Component.GetBehaviors().size()); }));
	return Descriptor;
}
} // namespace

void RegisterCoreComponentReflection(ReflectionRegistry &Registry)
{
	Registry.Register(MakeIdentityDescriptor());
	Registry.Register(MakeTransformDescriptor());
	Registry.Register(MakeHierarchyDescriptor());
	Registry.Register(MakeCameraDescriptor());
	Registry.Register(MakeMeshDescriptor());
	Registry.Register(MakePointLightDescriptor());
	Registry.Register(MakeSpotLightDescriptor());
	Registry.Register(MakeDirectionalLightDescriptor());
	Registry.Register(MakeAnimationDescriptor());
	Registry.Register(MakeBehaviorDescriptor());
}
} // namespace editor::reflection
