#include "RuntimeSceneLoader.h"

#include "src/core/io/SecurePath.h"

#include "RuntimeSceneBinary.h"

#include "src/component/object/CObjectAnimationComponent.h"
#include "src/component/object/CObjectBehaviorComponent.h"
#include "src/component/object/CObjectCameraComponent.h"
#include "src/component/object/CObjectHierarchyComponent.h"
#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectLightComponents.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace runtime::project
{
namespace
{
using Json = nlohmann::json;
constexpr uint64 MaximumRuntimeSceneBytes = 256ULL * 1'024ULL * 1'024ULL;
constexpr usize MaximumRuntimeObjects = 1'000'000U;
constexpr usize MaximumComponentsPerObject = 64U;
constexpr usize MaximumObjectNameBytes = 4U * 1'024U;

template <typename VectorType, usize ElementCount> [[nodiscard]] VectorType ReadVector(const Json &Value, const string_view Context)
{
	if (!Value.is_array() || Value.size() != ElementCount)
		throw RuntimeSceneLoadException(string(Context) + " must contain " + std::to_string(ElementCount) + " numbers");
	VectorType Result{};
	for (usize Index = 0; Index < ElementCount; ++Index)
	{
		if (!Value[Index].is_number())
			throw RuntimeSceneLoadException(string(Context) + " contains a non-numeric element");
		const float32 Element = Value[Index].get<float32>();
		if (!std::isfinite(Element))
			throw RuntimeSceneLoadException(string(Context) + " contains a non-finite element");
		Result[static_cast<typename VectorType::length_type>(Index)] = Element;
	}
	return Result;
}

[[nodiscard]] const Json &GetProperties(const Json &Components, const string_view ComponentName)
{
	const Json &Node = Components.at(string(ComponentName));
	if (!Node.is_object() || !Node.contains("Properties") || !Node.at("Properties").is_object())
		throw RuntimeSceneLoadException(string(ComponentName) + " component has an invalid property object");
	return Node.at("Properties");
}

template <IsCObjectComponent ComponentType>
void RestoreEnabled(world::Scene &Scene, const world::ComponentHandle<ComponentType> Handle, const Json &Components)
{
	auto Access = Scene.Write();
	Access.Resolve(Handle).SetEnabled(Components.at(string(ComponentType::ComponentName)).value("Enabled", true));
}

void RestoreShadowSettings(components::LightShadowSettings &Settings, const Json &Properties, const string_view ComponentName)
{
	Settings.CastShadows = Properties.value("CastShadows", Settings.CastShadows);
	const uint32 Resolution = Properties.value("ShadowResolution", static_cast<uint32>(Settings.Resolution));
	switch (Resolution)
	{
	case 256:
	case 512:
	case 1'024:
	case 2'048:
	case 4'096:
	case 8'192:
		Settings.Resolution = static_cast<components::ShadowResolution>(Resolution);
		break;
	default:
		throw RuntimeSceneLoadException(string(ComponentName) + ".ShadowResolution is unsupported");
	}
	Settings.ConstantBias = Properties.value("ShadowConstantBias", Settings.ConstantBias);
	Settings.SlopeBias = Properties.value("ShadowSlopeBias", Settings.SlopeBias);
	Settings.NormalBias = Properties.value("ShadowNormalBias", Settings.NormalBias);
	Settings.FilterRadius = Properties.value("ShadowFilterRadius", Settings.FilterRadius);
}

[[nodiscard]] components::BehaviorPropertyValue ReadBehaviorValue(const Json &Node)
{
	const uint32 Variant = Node.at("Variant").get<uint32>();
	const Json &Value = Node.at("Value");
	switch (Variant)
	{
	case 0:
		return Value.get<bool>();
	case 1:
		return Value.get<int32>();
	case 2:
		return Value.get<uint32>();
	case 3:
		return Value.get<int64>();
	case 4:
		return Value.get<uint64>();
	case 5:
		return Value.get<float32>();
	case 6:
		return Value.get<float64>();
	case 7:
		return Value.get<string>();
	case 9:
		return ReadVector<glm::vec2, 2>(Value, "behavior vec2");
	case 10:
		return ReadVector<glm::vec3, 3>(Value, "behavior vec3");
	case 11:
		return ReadVector<glm::vec4, 4>(Value, "behavior vec4");
	case 12:
	{
		const glm::vec4 Components = ReadVector<glm::vec4, 4>(Value, "behavior quaternion");
		return glm::quat(Components.w, Components.x, Components.y, Components.z);
	}
	case 13:
		return util::UUID::Parse(Value.get<string>());
	default:
		throw RuntimeSceneLoadException("Behavior property uses an unknown variant");
	}
}

void ValidateComponentNames(const Json &Components)
{
	static const std::unordered_set<string> Supported{
		string(components::CObjectIdentityComponent::ComponentName),		 string(components::CObjectTransformComponent::ComponentName),
		string(components::CObjectHierarchyComponent::ComponentName),		 string(components::CObjectCameraComponent::ComponentName),
		string(components::CObjectPointLightComponent::ComponentName),		 string(components::CObjectSpotLightComponent::ComponentName),
		string(components::CObjectDirectionalLightComponent::ComponentName), string(components::CObjectMeshComponent::ComponentName),
		string(components::CObjectAnimationComponent::ComponentName),		 string(components::CObjectBehaviorComponent::ComponentName)};
	for (const auto &[Name, Node] : Components.items())
	{
		(void)Node;
		if (!Supported.contains(Name))
			throw RuntimeSceneLoadException("Runtime scene contains unsupported component type '" + Name + "'");
	}
}

void ValidateRuntimeDocument(const Json &Root)
{
	if (!Root.is_object() || Root.value("FormatVersion", uint32{0}) != RuntimeSceneLoader::CurrentFormatVersion || !Root.contains("ID") ||
		!Root.at("ID").is_string() || !util::UUID::TryParse(Root.at("ID").get_ref<const string &>()).has_value() ||
		!Root.contains("Name") || !Root.at("Name").is_string() || Root.at("Name").get_ref<const string &>().empty() ||
		Root.at("Name").get_ref<const string &>().size() > MaximumObjectNameBytes || !Root.contains("Objects") ||
		!Root.at("Objects").is_array() || Root.at("Objects").size() > MaximumRuntimeObjects)
	{
		throw RuntimeSceneLoadException("Runtime scene root, identity, version, name, or object table is invalid");
	}
	std::unordered_set<util::UUID> Identities;
	Identities.reserve(Root.at("Objects").size());
	for (const Json &Object : Root.at("Objects"))
	{
		if (!Object.is_object() || !Object.contains("ID") || !Object.at("ID").is_string() || !Object.contains("Name") ||
			!Object.at("Name").is_string() || Object.at("Name").get_ref<const string &>().empty() ||
			Object.at("Name").get_ref<const string &>().size() > MaximumObjectNameBytes || !Object.contains("Components") ||
			!Object.at("Components").is_object() || Object.at("Components").size() > MaximumComponentsPerObject)
		{
			throw RuntimeSceneLoadException("Runtime scene contains an invalid object record");
		}
		const auto Identity = util::UUID::TryParse(Object.at("ID").get_ref<const string &>());
		if (!Identity.has_value() || !Identities.emplace(*Identity).second)
			throw RuntimeSceneLoadException("Runtime scene contains an invalid or duplicate object identity");
		ValidateComponentNames(Object.at("Components"));
		for (const auto &[Name, Component] : Object.at("Components").items())
		{
			(void)Name;
			if (!Component.is_object() || !Component.contains("Properties") || !Component.at("Properties").is_object())
				throw RuntimeSceneLoadException("Runtime scene contains an invalid component record");
		}
	}
}
} // namespace

LoadedRuntimeScene RuntimeSceneLoader::Load(const std::filesystem::path &Path, resource::AssetManager &Assets)
{
	const std::filesystem::path CanonicalPath = std::filesystem::absolute(Path).lexically_normal();
	try
	{
		const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileWithin(CanonicalPath.parent_path(), CanonicalPath.filename(),
																			  MaximumRuntimeSceneBytes, "Runtime scene");
		const string JsonSource = RuntimeSceneBinary::IsBinary(Bytes) ? RuntimeSceneBinary::DecodeToJson(Bytes)
																	  : string(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
		const Json Root = Json::parse(JsonSource, nullptr, true, true);
		ValidateRuntimeDocument(Root);

		LoadedRuntimeScene Result{.ID = util::UUID::Parse(Root.at("ID").get<string>()),
								  .Name = Root.at("Name").get<string>(),
								  .Scene = std::make_unique<world::Scene>()};
		if (!Result.ID.IsValid() || Result.Name.empty())
			throw RuntimeSceneLoadException("Runtime scene requires a valid identity and name");

		std::unordered_map<util::UUID, world::ObjectHandle> Objects;
		for (const Json &ObjectNode : Root.at("Objects"))
		{
			const util::UUID ID = util::UUID::Parse(ObjectNode.at("ID").get<string>());
			const string Name = ObjectNode.at("Name").get<string>();
			if (!ID.IsValid() || Name.empty() || Objects.contains(ID))
				throw RuntimeSceneLoadException("Runtime scene object identity or name is invalid or duplicated");
			const world::ObjectHandle Object = Result.Scene->CreateObject();
			try
			{
				(void)Result.Scene->AddComponent<components::CObjectIdentityComponent>(Object, Name, ID);
				(void)Result.Scene->AddComponent<components::CObjectTransformComponent>(Object);
				(void)Result.Scene->AddComponent<components::CObjectHierarchyComponent>(Object);
			}
			catch (...)
			{
				Result.Scene->DestroyObject(Object);
				throw;
			}
			Objects.emplace(ID, Object);
		}

		for (const Json &ObjectNode : Root.at("Objects"))
		{
			const util::UUID ID = util::UUID::Parse(ObjectNode.at("ID").get<string>());
			const world::ObjectHandle Object = Objects.at(ID);
			const Json &Components = ObjectNode.at("Components");
			if (!Components.is_object())
				throw RuntimeSceneLoadException("Runtime scene object Components must be an object");
			ValidateComponentNames(Components);
			const auto Has = [&Components](const string_view Name) { return Components.contains(string(Name)); };
			if (!Has(components::CObjectIdentityComponent::ComponentName) || !Has(components::CObjectTransformComponent::ComponentName) ||
				!Has(components::CObjectHierarchyComponent::ComponentName))
			{
				throw RuntimeSceneLoadException("Runtime scene object is missing an identity, transform, or hierarchy component");
			}

			const auto Identity = Result.Scene->GetComponent<components::CObjectIdentityComponent>(Object);
			const auto Transform = Result.Scene->GetComponent<components::CObjectTransformComponent>(Object);
			const auto Hierarchy = Result.Scene->GetComponent<components::CObjectHierarchyComponent>(Object);
			{
				auto Access = Result.Scene->Write();
				const Json &IdentityProperties = GetProperties(Components, components::CObjectIdentityComponent::ComponentName);
				components::CObjectIdentityComponent &IdentityComponent = Access.Resolve(Identity);
				IdentityComponent.SetName(IdentityProperties.value("Name", ObjectNode.at("Name").get<string>()));
				IdentityComponent.SetTags(IdentityProperties.value("Tags", std::vector<string>{}));
				IdentityComponent.SetMobility(static_cast<components::ObjectMobility>(
					IdentityProperties.value("Mobility", static_cast<uint32>(components::ObjectMobility::Movable))));
				IdentityComponent.SetEditorVisible(IdentityProperties.value("EditorVisible", true));
				IdentityComponent.SetLocked(IdentityProperties.value("Locked", false));
				const Json &TransformProperties = GetProperties(Components, components::CObjectTransformComponent::ComponentName);
				Access.Resolve(Transform).SetTransform(
					ReadVector<glm::vec3, 3>(TransformProperties.at("Position"), "Transform.Position"),
					glm::quat(glm::radians(ReadVector<glm::vec3, 3>(TransformProperties.at("RotationEuler"), "Transform.RotationEuler"))),
					ReadVector<glm::vec3, 3>(TransformProperties.at("Scale"), "Transform.Scale"));
			}
			RestoreEnabled(*Result.Scene, Identity, Components);
			RestoreEnabled(*Result.Scene, Transform, Components);
			RestoreEnabled(*Result.Scene, Hierarchy, Components);

			if (Has(components::CObjectCameraComponent::ComponentName))
			{
				const auto Handle = Result.Scene->AddComponent<components::CObjectCameraComponent>(Object);
				const Json &Properties = GetProperties(Components, components::CObjectCameraComponent::ComponentName);
				auto Access = Result.Scene->Write();
				components::CObjectCameraComponent &Component = Access.Resolve(Handle);
				const uint32 Projection = Properties.value("Projection", uint32{0});
				if (Projection > static_cast<uint32>(components::CameraProjection::Orthographic))
					throw RuntimeSceneLoadException("Camera.Projection is invalid");
				Component.SetProjection(static_cast<components::CameraProjection>(Projection));
				Component.SetVerticalFieldOfViewDegrees(Properties.value("VerticalFieldOfViewDegrees", 60.0f));
				Component.SetOrthographicHeight(Properties.value("OrthographicHeight", 10.0f));
				Component.SetClipPlanes(Properties.value("NearPlane", 0.05f), Properties.value("FarPlane", 100'000.0f));
				Component.SetExposureCompensation(Properties.value("ExposureCompensation", 0.0f));
				Component.SetPrimary(Properties.value("Primary", false));
				Component.SetTemporalJitterEnabled(Properties.value("TemporalJitterEnabled", true));
				Component.SetEnabled(Components.at(string(components::CObjectCameraComponent::ComponentName)).value("Enabled", true));
			}
			if (Has(components::CObjectPointLightComponent::ComponentName))
			{
				const auto Handle = Result.Scene->AddComponent<components::CObjectPointLightComponent>(Object);
				const Json &Properties = GetProperties(Components, components::CObjectPointLightComponent::ComponentName);
				auto Access = Result.Scene->Write();
				components::CObjectPointLightComponent &Component = Access.Resolve(Handle);
				Component.SetColor(ReadVector<glm::vec3, 3>(Properties.at("Color"), "PointLight.Color"));
				Component.SetLuminousPowerLumens(Properties.value("LuminousPowerLumens", 1'500.0f));
				Component.SetRange(Properties.value("Range", 20.0f));
				Component.SetSourceRadius(Properties.value("SourceRadius", 0.0f));
				RestoreShadowSettings(Component.GetShadowSettings(), Properties, components::CObjectPointLightComponent::ComponentName);
				Component.SetEnabled(Components.at(string(components::CObjectPointLightComponent::ComponentName)).value("Enabled", true));
			}
			if (Has(components::CObjectSpotLightComponent::ComponentName))
			{
				const auto Handle = Result.Scene->AddComponent<components::CObjectSpotLightComponent>(Object);
				const Json &Properties = GetProperties(Components, components::CObjectSpotLightComponent::ComponentName);
				auto Access = Result.Scene->Write();
				components::CObjectSpotLightComponent &Component = Access.Resolve(Handle);
				Component.SetColor(ReadVector<glm::vec3, 3>(Properties.at("Color"), "SpotLight.Color"));
				Component.SetLuminousPowerLumens(Properties.value("LuminousPowerLumens", 2'000.0f));
				Component.SetRange(Properties.value("Range", 30.0f));
				Component.SetConeAngles(Properties.value("InnerConeDegrees", 25.0f), Properties.value("OuterConeDegrees", 35.0f));
				RestoreShadowSettings(Component.GetShadowSettings(), Properties, components::CObjectSpotLightComponent::ComponentName);
				Component.SetEnabled(Components.at(string(components::CObjectSpotLightComponent::ComponentName)).value("Enabled", true));
			}
			if (Has(components::CObjectDirectionalLightComponent::ComponentName))
			{
				const auto Handle = Result.Scene->AddComponent<components::CObjectDirectionalLightComponent>(Object);
				const Json &Properties = GetProperties(Components, components::CObjectDirectionalLightComponent::ComponentName);
				auto Access = Result.Scene->Write();
				components::CObjectDirectionalLightComponent &Component = Access.Resolve(Handle);
				Component.SetColor(ReadVector<glm::vec3, 3>(Properties.at("Color"), "DirectionalLight.Color"));
				Component.SetIlluminanceLux(Properties.value("IlluminanceLux", 110'000.0f));
				Component.SetAngularDiameterDegrees(Properties.value("AngularDiameterDegrees", 0.5357f));
				Component.SetCascadeCount(Properties.value("CascadeCount", uint32{4}));
				Component.SetCascadeDistributionExponent(Properties.value("CascadeDistributionExponent", 2.0f));
				RestoreShadowSettings(Component.GetShadowSettings(), Properties,
									  components::CObjectDirectionalLightComponent::ComponentName);
				Component.SetEnabled(
					Components.at(string(components::CObjectDirectionalLightComponent::ComponentName)).value("Enabled", true));
			}
			if (Has(components::CObjectMeshComponent::ComponentName))
			{
				const Json &Node = Components.at(string(components::CObjectMeshComponent::ComponentName));
				const Json &Properties = GetProperties(Components, components::CObjectMeshComponent::ComponentName);
				const Json &Model = Properties.at("Model");
				if (Model.is_null())
					throw RuntimeSceneLoadException("Mesh.Model cannot be null");
				const auto Handle = Result.Scene->AddComponent<components::CObjectMeshComponent>(
					Object, Assets.GetAsset<resource::ModelAsset>(Model.at("Path").get<string>()));
				auto Access = Result.Scene->Write();
				components::CObjectMeshComponent &Component = Access.Resolve(Handle);
				Component.SetVisibility(static_cast<components::MeshVisibilityFlags>(Properties.value("Visibility", uint32{15})));
				components::MeshLODPolicy LOD;
				const uint32 LODMode = Properties.value("LODMode", uint32{0});
				if (LODMode > static_cast<uint32>(components::MeshLODSelectionMode::Forced))
					throw RuntimeSceneLoadException("Mesh.LODMode is invalid");
				LOD.Mode = static_cast<components::MeshLODSelectionMode>(LODMode);
				LOD.Bias = Properties.value("LODBias", int32{0});
				LOD.ForcedLOD = Properties.value("ForcedLOD", uint32{0});
				Component.SetLODPolicy(LOD);
				Component.SetRenderLayerMask(Properties.value("RenderLayerMask", ~uint32{0}));
				for (const Json &Override : Node.value("MaterialOverrides", Json::array()))
				{
					const auto Type = static_cast<resource::AssetType>(Override.at("Type").get<uint32>());
					Component.SetMaterialOverride(
						Override.at("MeshInstance").get<resource::ModelMeshInstanceID>(),
						Override.at("MaterialSlot").get<resource::MaterialSlotID>(),
						Assets.GetAsset<resource::MaterialInterfaceAsset>(Type, Override.at("Path").get<string>()));
				}
				Component.SetEnabled(Node.value("Enabled", true));
			}
			if (Has(components::CObjectAnimationComponent::ComponentName))
			{
				const Json &Node = Components.at(string(components::CObjectAnimationComponent::ComponentName));
				const Json &Properties = GetProperties(Components, components::CObjectAnimationComponent::ComponentName);
				const Json &Graph = Properties.at("Graph");
				if (Graph.is_null())
					throw RuntimeSceneLoadException("Animation.Graph cannot be null");
				const auto Handle = Result.Scene->AddComponent<components::CObjectAnimationComponent>(
					Object, Assets.GetAsset<resource::AnimationGraphAsset>(Graph.at("Path").get<string>()));
				auto Access = Result.Scene->Write();
				components::CObjectAnimationComponent &Component = Access.Resolve(Handle);
				const uint32 UpdateMode = Properties.value("UpdateMode", uint32{1});
				if (UpdateMode > static_cast<uint32>(components::AnimationUpdateMode::FixedRate))
					throw RuntimeSceneLoadException("Animation.UpdateMode is invalid");
				Component.SetUpdateMode(static_cast<components::AnimationUpdateMode>(UpdateMode));
				Component.SetRootMotionEnabled(Properties.value("RootMotionEnabled", false));
				for (const Json &Parameter : Node.value("Parameters", Json::array()))
				{
					Component.SetParameter(Parameter.at("ID").get<resource::AnimationParameterID>(),
										   static_cast<resource::AnimationParameterType>(Parameter.at("Type").get<uint32>()),
										   ReadVector<glm::vec4, 4>(Parameter.at("Value"), "Animation.Parameter"));
				}
				for (const Json &Morph : Node.value("MorphWeights", Json::array()))
					Component.SetMorphWeight(Morph.value("MorphSet", resource::AssetID{}),
											 Morph.at("Target").get<resource::MorphTargetID>(), Morph.at("Weight").get<float32>());
				for (const Json &Profile : Node.value("RetargetProfiles", Json::array()))
					Component.SetRetargetProfile(Assets.GetAsset<resource::RetargetProfileAsset>(Profile.get<string>()));
				Component.SetEnabled(Node.value("Enabled", true));
			}
			if (Has(components::CObjectBehaviorComponent::ComponentName))
			{
				const Json &Node = Components.at(string(components::CObjectBehaviorComponent::ComponentName));
				const auto Handle = Result.Scene->AddComponent<components::CObjectBehaviorComponent>(Object);
				auto Access = Result.Scene->Write();
				components::CObjectBehaviorComponent &Component = Access.Resolve(Handle);
				for (const Json &BehaviorNode : Node.value("Behaviors", Json::array()))
				{
					components::BehaviorInstance Behavior{.InstanceID = util::UUID::Parse(BehaviorNode.at("InstanceID").get<string>()),
														  .Type = BehaviorNode.at("Type").get<components::BehaviorTypeID>(),
														  .TypeName = BehaviorNode.at("TypeName").get<string>(),
														  .ModuleName = BehaviorNode.value("ModuleName", string("Engine")),
														  .StableTypeID =
															  BehaviorNode.contains("StableTypeID")
																  ? util::UUID::Parse(BehaviorNode.at("StableTypeID").get<string>())
																  : util::UUID{},
														  .SchemaVersion = BehaviorNode.at("SchemaVersion").get<uint32>(),
														  .Enabled = BehaviorNode.value("Enabled", true)};
					for (const auto &[Name, Value] : BehaviorNode.at("Properties").items())
						Behavior.Properties.emplace(Name, ReadBehaviorValue(Value));
					(void)Component.AddBehavior(std::move(Behavior));
				}
				Component.SetEnabled(Node.value("Enabled", true));
			}
		}

		for (const Json &ObjectNode : Root.at("Objects"))
		{
			if (ObjectNode.at("Parent").is_null())
				continue;
			const util::UUID ObjectID = util::UUID::Parse(ObjectNode.at("ID").get<string>());
			const util::UUID ParentID = util::UUID::Parse(ObjectNode.at("Parent").get<string>());
			const auto Object = Objects.find(ObjectID);
			const auto Parent = Objects.find(ParentID);
			if (Object == Objects.end() || Parent == Objects.end())
				throw RuntimeSceneLoadException("Runtime scene hierarchy references an unknown object");
			Result.Scene->SetParent(Object->second, Parent->second, ObjectNode.value("SiblingOrder", uint32{0}));
		}
		return Result;
	}
	catch (const RuntimeSceneLoadException &)
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		throw RuntimeSceneLoadException("Could not load runtime scene '" + CanonicalPath.string() + "': " + Exception.what());
	}
}
} // namespace runtime::project
