#include "SceneDocumentSerializer.h"

#include "Source/core/io/CompressedArchive.h"
#include "Source/core/io/SecurePath.h"

#include "Source/component/object/CObjectAnimationComponent.h"
#include "Source/component/object/CObjectBehaviorComponent.h"
#include "Source/component/object/CObjectCameraComponent.h"
#include "Source/component/object/CObjectHierarchyComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectLightComponents.h"
#include "Source/component/object/CObjectMeshComponent.h"
#include "Source/component/object/CObjectTransformComponent.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace editor::serialization
{
namespace
{
using Json = nlohmann::json;

constexpr uint64 MaximumDocumentBytes = 64ULL * 1'024ULL * 1'024ULL;
constexpr usize MaximumJsonNodes = 2'000'000;
constexpr usize MaximumJsonDepth = 128;
constexpr usize MaximumStringBytes = 4ULL * 1'024ULL * 1'024ULL;
constexpr usize MaximumObjectCount = 262'144;

[[nodiscard]] bool IsConcreteAssetType(const resource::AssetType Type) noexcept
{
	return static_cast<usize>(Type) < static_cast<usize>(resource::AssetType::Count);
}

void ValidateJsonBudget(const Json &Root)
{
	struct PendingNode final
	{
		const Json *Value = nullptr;
		usize Depth = 0;
	};
	std::vector<PendingNode> Pending{{.Value = &Root, .Depth = 1}};
	usize NodeCount = 0;
	while (!Pending.empty())
	{
		const PendingNode Current = Pending.back();
		Pending.pop_back();
		if (++NodeCount > MaximumJsonNodes || Current.Depth > MaximumJsonDepth)
			throw SceneDocumentSerializationException("Scene document exceeds the supported JSON node or nesting budget");
		if (Current.Value->is_string() && Current.Value->get_ref<const string &>().size() > MaximumStringBytes)
			throw SceneDocumentSerializationException("Scene document contains a string that exceeds the supported size budget");
		if (Current.Value->is_array() || Current.Value->is_object())
		{
			for (const Json &Child : *Current.Value)
				Pending.push_back({.Value = &Child, .Depth = Current.Depth + 1});
		}
	}
}

[[nodiscard]] const std::array<string_view, 10> &KnownComponentNames()
{
	static constexpr std::array Names{
		components::CObjectIdentityComponent::ComponentName,		 components::CObjectTransformComponent::ComponentName,
		components::CObjectHierarchyComponent::ComponentName,		 components::CObjectCameraComponent::ComponentName,
		components::CObjectPointLightComponent::ComponentName,		 components::CObjectSpotLightComponent::ComponentName,
		components::CObjectDirectionalLightComponent::ComponentName, components::CObjectMeshComponent::ComponentName,
		components::CObjectAnimationComponent::ComponentName,		 components::CObjectBehaviorComponent::ComponentName};
	return Names;
}

[[nodiscard]] bool IsKnownComponent(const string_view Name)
{
	return std::ranges::find(KnownComponentNames(), Name) != KnownComponentNames().end();
}

void ValidateSerializedRoot(const Json &Root)
{
	ValidateJsonBudget(Root);
	if (!Root.is_object() || !Root.contains("FormatVersion") || !Root.at("FormatVersion").is_number_unsigned())
		throw SceneDocumentSerializationException("Scene document does not declare a valid format version");
	if (Root.at("FormatVersion").get<uint32>() != SceneDocumentSerializer::CurrentFormatVersion)
		throw SceneDocumentSerializationException("Scene document uses an incompatible pre-instance format; format version 2 is required");
	if (!Root.is_object() || !Root.contains("FormatVersion") || !Root.at("FormatVersion").is_number_unsigned() ||
		Root.at("FormatVersion").get<uint32>() != SceneDocumentSerializer::CurrentFormatVersion || !Root.contains("EngineSchemaVersion") ||
		!Root.at("EngineSchemaVersion").is_number_unsigned() || Root.at("EngineSchemaVersion").get<uint32>() == 0 ||
		Root.at("EngineSchemaVersion").get<uint32>() > SceneDocumentSerializer::CurrentEngineSchemaVersion || !Root.contains("ID") ||
		!Root.at("ID").is_string() || !util::UUID::Parse(Root.at("ID").get<string>()).IsValid() || !Root.contains("Name") ||
		!Root.at("Name").is_string() || Root.at("Name").get_ref<const string &>().empty() || !Root.contains("Objects") ||
		!Root.at("Objects").is_array() || Root.at("Objects").size() > MaximumObjectCount || !Root.contains("Instances") ||
		!Root.at("Instances").is_array() || Root.at("Instances").size() > MaximumObjectCount)
	{
		throw SceneDocumentSerializationException("Scene document root, version, identity, name, or object array is invalid");
	}
	std::unordered_set<util::UUID> IDs;
	std::unordered_map<util::UUID, util::UUID> Parents;
	for (const Json &Object : Root.at("Objects"))
	{
		if (!Object.is_object() || !Object.contains("ID") || !Object.at("ID").is_string() || !Object.contains("Name") ||
			!Object.at("Name").is_string() || Object.at("Name").get_ref<const string &>().empty() || !Object.contains("Components") ||
			!Object.at("Components").is_object())
		{
			throw SceneDocumentSerializationException("Scene document contains an invalid object record");
		}
		const util::UUID ID = util::UUID::Parse(Object.at("ID").get<string>());
		if (!ID.IsValid() || !IDs.emplace(ID).second)
			throw SceneDocumentSerializationException("Scene document contains an invalid or duplicate object identity");
		if (!Object.contains("Parent") || (!Object.at("Parent").is_null() && !Object.at("Parent").is_string()) ||
			!Object.contains("SiblingOrder") || !Object.at("SiblingOrder").is_number_unsigned())
			throw SceneDocumentSerializationException("Scene document contains invalid hierarchy metadata");
		util::UUID Parent;
		if (!Object.at("Parent").is_null())
		{
			Parent = util::UUID::Parse(Object.at("Parent").get<string>());
			if (!Parent.IsValid() || Parent == ID)
				throw SceneDocumentSerializationException("Scene document contains an invalid or self parent reference");
		}
		Parents.emplace(ID, Parent);
		const Json &Components = Object.at("Components");
		for (const string_view Required :
			 {components::CObjectIdentityComponent::ComponentName, components::CObjectTransformComponent::ComponentName,
			  components::CObjectHierarchyComponent::ComponentName})
		{
			if (!Components.contains(string(Required)))
				throw SceneDocumentSerializationException("Scene object is missing required component '" + string(Required) + "'");
		}
		for (const auto &[Name, Component] : Components.items())
		{
			if (!Component.is_object())
				throw SceneDocumentSerializationException("Scene component '" + Name + "' is not an object");
			if (!IsKnownComponent(Name))
				continue;
			if (!Component.contains("SchemaVersion") || !Component.at("SchemaVersion").is_number_unsigned() ||
				Component.at("SchemaVersion").get<uint32>() != SceneDocumentSerializer::CurrentComponentSchemaVersion ||
				!Component.contains("Properties") || !Component.at("Properties").is_object())
				throw SceneDocumentSerializationException("Scene component '" + Name + "' has an unsupported schema or properties node");
		}
	}
	for (const auto &[ID, Parent] : Parents)
	{
		if (Parent.IsValid() && !IDs.contains(Parent))
			throw SceneDocumentSerializationException("Scene document contains a missing parent reference");
		std::unordered_set<util::UUID> Chain;
		util::UUID Current = ID;
		while (Current.IsValid())
		{
			if (!Chain.emplace(Current).second)
				throw SceneDocumentSerializationException("Scene document hierarchy contains a cycle");
			Current = Parents.at(Current);
		}
	}
}

[[nodiscard]] Json MergePreservedData(Json Current, const string &PreservedData)
{
	if (PreservedData.empty())
		return Current;
	Json Preserved;
	try
	{
		Preserved = Json::parse(PreservedData);
	}
	catch (const nlohmann::json::exception &)
	{
		return Current;
	}
	if (!Preserved.is_object() || !Preserved.contains("Objects") || !Preserved.at("Objects").is_array())
		return Current;

	Json Result = Preserved;
	Result["FormatVersion"] = Current.at("FormatVersion");
	Result["EngineSchemaVersion"] = Current.at("EngineSchemaVersion");
	Result["MigrationData"] = Current.at("MigrationData");
	Result["ID"] = Current.at("ID");
	Result["Name"] = Current.at("Name");
	Result["Objects"] = Json::array();
	for (Json &CurrentObject : Current.at("Objects"))
	{
		const auto Existing = std::ranges::find_if(
			Preserved.at("Objects"), [&CurrentObject](const Json &Candidate)
			{ return Candidate.is_object() && Candidate.value("ID", string{}) == CurrentObject.at("ID").get<string>(); });
		Json MergedObject = Existing == Preserved.at("Objects").end() ? Json::object() : *Existing;
		MergedObject["ID"] = CurrentObject.at("ID");
		MergedObject["Name"] = CurrentObject.at("Name");
		MergedObject["Parent"] = CurrentObject.at("Parent");
		MergedObject["SiblingOrder"] = CurrentObject.at("SiblingOrder");
		Json MergedComponents = Json::object();
		const Json *ExistingComponents =
			Existing != Preserved.at("Objects").end() && Existing->contains("Components") && Existing->at("Components").is_object()
				? &Existing->at("Components")
				: nullptr;
		if (ExistingComponents != nullptr)
		{
			for (const auto &[Name, Node] : ExistingComponents->items())
				if (!IsKnownComponent(Name))
					MergedComponents[Name] = Node;
		}
		for (const auto &[Name, CurrentComponent] : CurrentObject.at("Components").items())
		{
			Json MergedComponent =
				ExistingComponents != nullptr && ExistingComponents->contains(Name) ? ExistingComponents->at(Name) : Json::object();
			for (const auto &[Key, Value] : CurrentComponent.items())
			{
				if (Key != "Properties")
					MergedComponent[Key] = Value;
			}
			Json Properties = MergedComponent.value("Properties", Json::object());
			if (!Properties.is_object())
				Properties = Json::object();
			for (const auto &[Property, Value] : CurrentComponent.at("Properties").items())
				Properties[Property] = Value;
			MergedComponent["Properties"] = std::move(Properties);
			MergedComponents[Name] = std::move(MergedComponent);
		}
		MergedObject["Components"] = std::move(MergedComponents);
		Result["Objects"].push_back(std::move(MergedObject));
	}
	return Result;
}

[[nodiscard]] Json VectorJson(const glm::vec2 &Value)
{
	if (!std::isfinite(Value.x) || !std::isfinite(Value.y))
		throw SceneDocumentSerializationException("Cannot serialize a non-finite vec2");
	return Json::array({Value.x, Value.y});
}

[[nodiscard]] Json VectorJson(const glm::vec3 &Value)
{
	if (!std::isfinite(Value.x) || !std::isfinite(Value.y) || !std::isfinite(Value.z))
		throw SceneDocumentSerializationException("Cannot serialize a non-finite vec3");
	return Json::array({Value.x, Value.y, Value.z});
}

[[nodiscard]] Json VectorJson(const glm::vec4 &Value)
{
	if (!std::isfinite(Value.x) || !std::isfinite(Value.y) || !std::isfinite(Value.z) || !std::isfinite(Value.w))
		throw SceneDocumentSerializationException("Cannot serialize a non-finite vec4");
	return Json::array({Value.x, Value.y, Value.z, Value.w});
}

[[nodiscard]] Json VectorJson(const glm::quat &Value)
{
	if (!std::isfinite(Value.x) || !std::isfinite(Value.y) || !std::isfinite(Value.z) || !std::isfinite(Value.w))
		throw SceneDocumentSerializationException("Cannot serialize a non-finite quaternion");
	return Json::array({Value.x, Value.y, Value.z, Value.w});
}

template <typename VectorType, usize ElementCount> [[nodiscard]] VectorType ReadVector(const Json &Value, const string_view Context)
{
	if (!Value.is_array() || Value.size() != ElementCount)
		throw SceneDocumentSerializationException(string(Context) + " must be an array of " + std::to_string(ElementCount) + " numbers");
	VectorType Result{};
	for (usize Index = 0; Index < ElementCount; ++Index)
	{
		if (!Value[Index].is_number())
			throw SceneDocumentSerializationException(string(Context) + " contains a non-numeric element");
		Result[static_cast<typename VectorType::length_type>(Index)] = Value[Index].get<float32>();
		if (!std::isfinite(Result[static_cast<typename VectorType::length_type>(Index)]))
			throw SceneDocumentSerializationException(string(Context) + " contains a non-finite element");
	}
	return Result;
}

[[nodiscard]] std::filesystem::path AssetPath(const resource::AssetManager &Assets, const resource::AssetID &ID)
{
	if (ID.empty())
		return {};
	const resource::AssetRecordHandle Record = Assets.GetRecord(ID);
	if (Record == nullptr)
		throw SceneDocumentSerializationException("Scene references unknown asset record '" + ID + "'");
	std::error_code Error;
	std::filesystem::path Relative = std::filesystem::relative(Record->GetCanonicalPath(), Assets.GetRootPath(), Error);
	if (Error || Relative.empty() || Relative.native().starts_with(L".."))
		throw SceneDocumentSerializationException("Scene asset is outside the project asset root: '" + Record->GetCanonicalPath().string() +
												  "'");
	return Relative.lexically_normal();
}

[[nodiscard]] Json PropertyToJson(const reflection::PropertyValue &Value, const resource::AssetManager &Assets,
								  const world::Scene::ReadAccess &Access)
{
	return std::visit(
		[&Assets, &Access]<typename ValueType>(const ValueType &Typed) -> Json
		{
			if constexpr (std::same_as<ValueType, glm::vec2> || std::same_as<ValueType, glm::vec3> || std::same_as<ValueType, glm::vec4> ||
						  std::same_as<ValueType, glm::quat>)
				return VectorJson(Typed);
			else if constexpr (std::same_as<ValueType, util::UUID>)
				return Typed.ToString();
			else if constexpr (std::same_as<ValueType, world::ObjectHandle>)
			{
				if (!Typed.IsValid())
					return nullptr;
				try
				{
					const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Typed);
					if (!Identity.IsValid())
						throw SceneDocumentSerializationException("Reflected object reference targets an object without identity");
					return Access.Resolve(Identity).GetPersistentID().ToString();
				}
				catch (const SceneDocumentSerializationException &)
				{
					throw;
				}
				catch (const std::exception &Exception)
				{
					throw SceneDocumentSerializationException(string("Reflected object reference is not part of the serialized scene: ") +
															  Exception.what());
				}
			}
			else if constexpr (std::same_as<ValueType, reflection::AssetReference>)
			{
				if (Typed.ID.empty())
					return nullptr;
				return Json{
					{"ID", Typed.ID}, {"Type", static_cast<uint32>(Typed.Type)}, {"Path", AssetPath(Assets, Typed.ID).generic_string()}};
			}
			else
				return Typed;
		},
		Value);
}

[[nodiscard]] reflection::AssetReference ReadAssetReference(const Json &Value, resource::AssetManager &Assets, const string_view Context)
{
	if (Value.is_null())
		return {};
	if (!Value.is_object() || (Value.contains("ID") && !Value.at("ID").is_string()) || !Value.contains("Type") ||
		!Value.at("Type").is_number_unsigned() || !Value.contains("Path") || !Value.at("Path").is_string())
		throw SceneDocumentSerializationException(string(Context) + " must be a typed asset reference");
	reflection::AssetReference Result{.ID = Value.value("ID", resource::AssetID{}),
									  .Type = static_cast<resource::AssetType>(Value.at("Type").get<uint32>())};
	const std::filesystem::path RelativePath = Value.at("Path").get<string>();
	if (!IsConcreteAssetType(Result.Type) || RelativePath.empty() || RelativePath.is_absolute())
		throw SceneDocumentSerializationException(string(Context) + " contains an invalid ID, type, or relative path");
	const std::filesystem::path CanonicalPath = Assets.ResolvePath(RelativePath);
	resource::AssetRecordHandle Record = Result.ID.empty() ? resource::AssetRecordHandle{} : Assets.GetRecord(Result.ID);
	if (Record == nullptr)
		Record = Assets.GetRecord(Result.Type, RelativePath);
	if (Record != nullptr &&
		((!Result.ID.empty() && Record->GetID() != Result.ID) || Record->GetType() != Result.Type ||
		 resource::AssetManager::CanonicalizePath(Record->GetCanonicalPath()) != resource::AssetManager::CanonicalizePath(CanonicalPath)))
		throw SceneDocumentSerializationException(string(Context) + " asset ID, type, and path do not identify the same record");
	if (Record != nullptr && Result.ID.empty())
		Result.ID = Record->GetID();
	return Result;
}

template <IsAssetWithStaticType AssetType>
[[nodiscard]] resource::AssetHandle<AssetType> LoadAssetReference(const Json &Value, resource::AssetManager &Assets,
																  const string_view Context)
{
	const reflection::AssetReference Reference = ReadAssetReference(Value, Assets, Context);
	if (Reference.Type != AssetType::AssetType)
		throw SceneDocumentSerializationException(string(Context) + " has the wrong concrete asset type");
	try
	{
		auto Asset = Assets.GetAsset<AssetType>(Value.at("Path").get<string>());
		if (!Asset || (!Reference.ID.empty() && Asset.GetID() != Reference.ID))
			throw SceneDocumentSerializationException(string(Context) + " loaded asset does not match its serialized identity");
		return Asset;
	}
	catch (const SceneDocumentSerializationException &)
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		throw SceneDocumentSerializationException(string(Context) + " could not resolve its asset: " + Exception.what());
	}
}

[[nodiscard]] reflection::PropertyValue PropertyFromJson(const Json &Value, const reflection::PropertyValue &Prototype,
														 const string_view Context,
														 const std::unordered_map<util::UUID, world::ObjectHandle> &Objects,
														 resource::AssetManager &Assets)
{
	return std::visit(
		[&Value, Context, &Objects, &Assets]<typename ValueType>(const ValueType &) -> reflection::PropertyValue
		{
			try
			{
				if constexpr (std::same_as<ValueType, glm::vec2>)
					return ReadVector<glm::vec2, 2>(Value, Context);
				else if constexpr (std::same_as<ValueType, glm::vec3>)
					return ReadVector<glm::vec3, 3>(Value, Context);
				else if constexpr (std::same_as<ValueType, glm::vec4>)
					return ReadVector<glm::vec4, 4>(Value, Context);
				else if constexpr (std::same_as<ValueType, glm::quat>)
				{
					const glm::vec4 Components = ReadVector<glm::vec4, 4>(Value, Context);
					return glm::quat(Components.w, Components.x, Components.y, Components.z);
				}
				else if constexpr (std::same_as<ValueType, util::UUID>)
					return util::UUID::Parse(Value.get<string>());
				else if constexpr (std::same_as<ValueType, world::ObjectHandle>)
				{
					if (Value.is_null())
						return world::ObjectHandle{};
					const util::UUID ID = util::UUID::Parse(Value.get<string>());
					const auto Object = Objects.find(ID);
					if (!ID.IsValid() || Object == Objects.end())
						throw SceneDocumentSerializationException(string(Context) + " references an unknown scene object");
					return Object->second;
				}
				else if constexpr (std::same_as<ValueType, reflection::AssetReference>)
					return ReadAssetReference(Value, Assets, Context);
				else
				{
					ValueType Result = Value.get<ValueType>();
					if constexpr (std::floating_point<ValueType>)
					{
						if (!std::isfinite(Result))
							throw SceneDocumentSerializationException(string(Context) + " contains a non-finite scalar");
					}
					return Result;
				}
			}
			catch (const nlohmann::json::exception &Exception)
			{
				throw SceneDocumentSerializationException(string(Context) + " has an incompatible value: " + Exception.what());
			}
		},
		Prototype);
}

[[nodiscard]] Json BehaviorValueToJson(const components::BehaviorPropertyValue &Value)
{
	return std::visit(
		[]<typename ValueType>(const ValueType &Typed) -> Json
		{
			if constexpr (std::same_as<ValueType, glm::vec2>)
				return Json{{"Value", VectorJson(Typed)}, {"Variant", 9U}};
			else if constexpr (std::same_as<ValueType, glm::vec3>)
				return Json{{"Value", VectorJson(Typed)}, {"Variant", 10U}};
			else if constexpr (std::same_as<ValueType, glm::vec4>)
				return Json{{"Value", VectorJson(Typed)}, {"Variant", 11U}};
			else if constexpr (std::same_as<ValueType, glm::quat>)
				return Json{{"Value", VectorJson(Typed)}, {"Variant", 12U}};
			else if constexpr (std::same_as<ValueType, util::UUID>)
				return Json{{"Value", Typed.ToString()}, {"Variant", 13U}};
			else
			{
				constexpr uint32 Variant = []()
				{
					if constexpr (std::same_as<ValueType, bool>)
						return 0U;
					else if constexpr (std::same_as<ValueType, int32>)
						return 1U;
					else if constexpr (std::same_as<ValueType, uint32>)
						return 2U;
					else if constexpr (std::same_as<ValueType, int64>)
						return 3U;
					else if constexpr (std::same_as<ValueType, uint64>)
						return 4U;
					else if constexpr (std::same_as<ValueType, float32>)
						return 5U;
					else if constexpr (std::same_as<ValueType, float64>)
						return 6U;
					else
						return 7U;
				}();
				return Json{{"Value", Typed}, {"Variant", Variant}};
			}
		},
		Value);
}

[[nodiscard]] components::BehaviorPropertyValue BehaviorValueFromJson(const Json &Node)
{
	if (!Node.is_object() || !Node.contains("Variant") || !Node.at("Variant").is_number_unsigned() || !Node.contains("Value"))
		throw SceneDocumentSerializationException("Behavior property is not a valid tagged value");
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
	{
		const float32 Result = Value.get<float32>();
		if (!std::isfinite(Result))
			throw SceneDocumentSerializationException("Behavior property contains a non-finite float32");
		return Result;
	}
	case 6:
	{
		const float64 Result = Value.get<float64>();
		if (!std::isfinite(Result))
			throw SceneDocumentSerializationException("Behavior property contains a non-finite float64");
		return Result;
	}
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
	{
		const util::UUID Result = util::UUID::Parse(Value.get<string>());
		if (!Result.IsValid())
			throw SceneDocumentSerializationException("Behavior property contains an invalid UUID");
		return Result;
	}
	default:
		throw SceneDocumentSerializationException("Behavior property uses an unknown variant");
	}
}

template <IsCObjectComponent ComponentType>
void SaveComponent(Json &Components, const world::Scene::ReadAccess &Access, const world::ObjectHandle Object,
				   const reflection::ReflectionRegistry &Reflection, const resource::AssetManager &Assets)
{
	const world::ComponentHandle<ComponentType> Handle = Access.GetComponent<ComponentType>(Object);
	if (!Handle.IsValid())
		return;
	const std::optional<reflection::TypeDescriptor> Descriptor = Reflection.Find("components." + string(ComponentType::ComponentName));
	if (!Descriptor.has_value())
		throw SceneDocumentSerializationException("No reflection descriptor exists for " + string(ComponentType::ComponentName));
	Json Node{{"Type", ComponentType::ComponentName}, {"SchemaVersion", 1U}, {"Properties", Json::object()}};
	const ComponentType &Component = Access.Resolve(Handle);
	Node["Enabled"] = Component.IsEnabled();
	for (const reflection::PropertyDescriptor &Property : Descriptor->Properties)
		Node["Properties"][Property.Name] = PropertyToJson(Property.Read(&Component), Assets, Access);
	Components[string(ComponentType::ComponentName)] = std::move(Node);
}

void ValidateReflectedValue(const reflection::PropertyDescriptor &Property, const reflection::PropertyValue &Value,
							const string_view Context)
{
	const auto NumericValue = std::visit(
		[]<typename ValueType>(const ValueType &Typed) -> std::optional<float64>
		{
			if constexpr (std::integral<ValueType> && !std::same_as<ValueType, bool>)
				return static_cast<float64>(Typed);
			else if constexpr (std::floating_point<ValueType>)
				return static_cast<float64>(Typed);
			else
				return std::nullopt;
		},
		Value);
	if (NumericValue.has_value())
	{
		if (!std::isfinite(*NumericValue) || (Property.Numeric.Minimum.has_value() && *NumericValue < *Property.Numeric.Minimum) ||
			(Property.Numeric.Maximum.has_value() && *NumericValue > *Property.Numeric.Maximum))
			throw SceneDocumentSerializationException(string(Context) + " violates its numeric constraints");
		if (!Property.EnumOptions.empty() && !reflection::HasFlag(Property.Flags, reflection::PropertyFlags::Bitmask))
		{
			const uint64 Candidate = static_cast<uint64>(*NumericValue);
			if (std::ranges::none_of(Property.EnumOptions,
									 [Candidate](const reflection::EnumPropertyOption &Option) { return Option.Value == Candidate; }))
				throw SceneDocumentSerializationException(string(Context) + " uses an unsupported enumeration value");
		}
	}
	if (const auto *Rotation = std::get_if<glm::quat>(&Value))
	{
		const float64 NormSquared = static_cast<float64>(Rotation->w) * Rotation->w + static_cast<float64>(Rotation->x) * Rotation->x +
									static_cast<float64>(Rotation->y) * Rotation->y + static_cast<float64>(Rotation->z) * Rotation->z;
		if (!std::isfinite(NormSquared) || NormSquared <= std::numeric_limits<float64>::epsilon())
			throw SceneDocumentSerializationException(string(Context) + " contains an invalid quaternion");
	}
}

template <IsCObjectComponent ComponentType>
void ApplyComponentProperties(document::SceneDocument &Document, const world::ObjectHandle Object, const Json &Node,
							  const reflection::ReflectionRegistry &Reflection, resource::AssetManager &Assets,
							  const std::unordered_map<util::UUID, world::ObjectHandle> &Objects)
{
	world::Scene &Scene = Document.GetScene();
	const world::ComponentHandle<ComponentType> Handle = Scene.GetComponent<ComponentType>(Object);
	if (!Handle.IsValid())
		throw SceneDocumentSerializationException("Scene component was not constructed before property restoration");
	const std::optional<reflection::TypeDescriptor> Descriptor = Reflection.Find("components." + string(ComponentType::ComponentName));
	if (!Descriptor.has_value())
		throw SceneDocumentSerializationException("No reflection descriptor exists for " + string(ComponentType::ComponentName));
	const Json &Properties = Node.at("Properties");
	if (!Node.contains("Enabled") || !Node.at("Enabled").is_boolean())
		throw SceneDocumentSerializationException("Scene component Enabled state must be Boolean");
	auto Access = Scene.Write();
	ComponentType &Component = Access.Resolve(Handle);
	Component.SetEnabled(Node.value("Enabled", true));
	for (const reflection::PropertyDescriptor &Property : Descriptor->Properties)
	{
		if (!Property.Write || reflection::HasFlag(Property.Flags, reflection::PropertyFlags::ReadOnly) ||
			!Properties.contains(Property.Name))
			continue;
		const reflection::PropertyValue Prototype = Property.Read(&Component);
		const reflection::PropertyValue Restored = PropertyFromJson(
			Properties.at(Property.Name), Prototype, string(ComponentType::ComponentName) + "." + Property.Name, Objects, Assets);
		ValidateReflectedValue(Property, Restored, string(ComponentType::ComponentName) + "." + Property.Name);
		Property.Write(&Component, Restored, {.Scene = &Scene, .Assets = &Assets});
	}
}

void PreflightReflectedProperties(const Json &Root, const reflection::ReflectionRegistry &Reflection, resource::AssetManager &Assets)
{
	std::unordered_set<util::UUID> ObjectIDs;
	ObjectIDs.reserve(Root.at("Objects").size());
	for (const Json &Object : Root.at("Objects"))
		ObjectIDs.emplace(util::UUID::Parse(Object.at("ID").get<string>()));

	for (const Json &Object : Root.at("Objects"))
	{
		const Json &Components = Object.at("Components");
		const string ObjectID = Object.at("ID").get<string>();
		const Json &IdentityProperties = Components.at(string(components::CObjectIdentityComponent::ComponentName)).at("Properties");
		if (!IdentityProperties.contains("PersistentID") || !IdentityProperties.at("PersistentID").is_string() ||
			IdentityProperties.at("PersistentID").get<string>() != ObjectID || !IdentityProperties.contains("Name") ||
			!IdentityProperties.at("Name").is_string() || IdentityProperties.at("Name").get<string>() != Object.at("Name").get<string>())
			throw SceneDocumentSerializationException("Scene object identity properties disagree with the authoritative object record");
		const Json &HierarchyProperties = Components.at(string(components::CObjectHierarchyComponent::ComponentName)).at("Properties");
		if (!HierarchyProperties.contains("Parent") ||
			(HierarchyProperties.at("Parent") != Object.at("Parent") && !HierarchyProperties.at("Parent").is_null()) ||
			!HierarchyProperties.contains("SiblingOrder") || HierarchyProperties.at("SiblingOrder") != Object.at("SiblingOrder"))
			throw SceneDocumentSerializationException("Scene hierarchy properties disagree with the authoritative object record");

		for (const auto &[ComponentName, Component] : Components.items())
		{
			if (!IsKnownComponent(ComponentName))
				continue;
			const std::optional<reflection::TypeDescriptor> Descriptor = Reflection.Find("components." + ComponentName);
			if (!Descriptor.has_value())
				throw SceneDocumentSerializationException("No reflection descriptor exists for " + ComponentName);
			const Json &Properties = Component.at("Properties");
			for (const reflection::PropertyDescriptor &Property : Descriptor->Properties)
			{
				if (!Properties.contains(Property.Name))
					continue;
				const Json &Value = Properties.at(Property.Name);
				const string Context = ComponentName + "." + Property.Name;
				if (Property.Kind == reflection::PropertyKind::ObjectReference)
				{
					if (Value.is_null())
						continue;
					if (!Value.is_string())
						throw SceneDocumentSerializationException(Context + " must be null or an object UUID");
					const util::UUID Reference = util::UUID::Parse(Value.get<string>());
					if (!Reference.IsValid() || !ObjectIDs.contains(Reference))
						throw SceneDocumentSerializationException(Context + " references an unknown object UUID");
					continue;
				}
				if (Property.Kind == reflection::PropertyKind::AssetReference)
				{
					(void)ReadAssetReference(Value, Assets, Context);
					continue;
				}
				if (!Property.DefaultValue.has_value())
					continue;
				const reflection::PropertyValue Restored = PropertyFromJson(Value, *Property.DefaultValue, Context, {}, Assets);
				ValidateReflectedValue(Property, Restored, Context);
			}
		}
	}
}

[[nodiscard]] Json ReadJsonFile(const std::filesystem::path &Path)
{
	try
	{
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(Path.parent_path(), Path.filename(), MaximumDocumentBytes, "Scene document");
		usize ParsedNodes = 0;
		const auto BudgetCallback = [&ParsedNodes](const int Depth, const Json::parse_event_t Event, Json &Parsed)
		{
			if (Depth < 0 || static_cast<usize>(Depth) > MaximumJsonDepth)
				throw SceneDocumentSerializationException("Scene document exceeds the supported JSON nesting budget");
			if (Event == Json::parse_event_t::object_start || Event == Json::parse_event_t::array_start ||
				Event == Json::parse_event_t::value)
			{
				if (++ParsedNodes > MaximumJsonNodes)
					throw SceneDocumentSerializationException("Scene document exceeds the supported JSON node budget");
			}
			if (Event == Json::parse_event_t::value && Parsed.is_string() && Parsed.get_ref<const string &>().size() > MaximumStringBytes)
				throw SceneDocumentSerializationException("Scene document contains a string that exceeds the supported size budget");
			return true;
		};
		return Json::parse(Bytes.begin(), Bytes.end(), BudgetCallback, true, true);
	}
	catch (const nlohmann::json::exception &Exception)
	{
		throw SceneDocumentSerializationException("Could not parse scene document '" + Path.string() + "': " + Exception.what());
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw SceneDocumentSerializationException("Could not securely read scene document '" + Path.string() + "': " + Exception.what());
	}
}

} // namespace

void SceneDocumentMigrationRegistry::RegisterDocumentMigration(const uint32 FromFormatVersion, const uint32 FromEngineSchemaVersion,
															   const uint32 ToFormatVersion, const uint32 ToEngineSchemaVersion,
															   MigrationFunction Migration)
{
	if (ToFormatVersion == 0 || ToEngineSchemaVersion == 0 || !Migration)
		throw std::invalid_argument("Scene document migration requires non-zero target versions and a callback");
	if (ToFormatVersion < FromFormatVersion || (ToFormatVersion == FromFormatVersion && ToEngineSchemaVersion <= FromEngineSchemaVersion))
	{
		throw std::invalid_argument("Scene document migration versions must advance monotonically");
	}
	std::unique_lock Lock(this->Mutex);
	if (!this->DocumentMigrations
			 .emplace(std::pair{FromFormatVersion, FromEngineSchemaVersion},
					  DocumentMigration{.ToFormatVersion = ToFormatVersion,
										.ToEngineSchemaVersion = ToEngineSchemaVersion,
										.Apply = std::move(Migration)})
			 .second)
	{
		throw std::invalid_argument("A scene document migration is already registered for the source version");
	}
}

void SceneDocumentMigrationRegistry::RegisterComponentMigration(string ComponentName, const uint32 FromSchemaVersion,
																const uint32 ToSchemaVersion, MigrationFunction Migration)
{
	if (ComponentName.empty() || ToSchemaVersion <= FromSchemaVersion || !Migration)
		throw std::invalid_argument("Scene component migration requires a name, advancing versions, and a callback");
	std::unique_lock Lock(this->Mutex);
	if (!this->ComponentMigrations
			 .emplace(std::pair{std::move(ComponentName), FromSchemaVersion},
					  ComponentMigration{.ToSchemaVersion = ToSchemaVersion, .Apply = std::move(Migration)})
			 .second)
	{
		throw std::invalid_argument("A scene component migration is already registered for the source schema");
	}
}

void SceneDocumentMigrationRegistry::Migrate(Json &Root, const uint32 TargetFormatVersion, const uint32 TargetEngineSchemaVersion,
											 const uint32 TargetComponentSchemaVersion) const
{
	ValidateJsonBudget(Root);
	if (!Root.is_object() || !Root.contains("FormatVersion") || !Root.at("FormatVersion").is_number_unsigned() ||
		!Root.contains("EngineSchemaVersion") || !Root.at("EngineSchemaVersion").is_number_unsigned())
	{
		throw SceneDocumentSerializationException("Scene document cannot be migrated because its root versions are missing or invalid");
	}
	uint32 FormatVersion = Root.at("FormatVersion").get<uint32>();
	uint32 EngineSchemaVersion = Root.at("EngineSchemaVersion").get<uint32>();
	while (FormatVersion != TargetFormatVersion || EngineSchemaVersion != TargetEngineSchemaVersion)
	{
		if (FormatVersion > TargetFormatVersion || EngineSchemaVersion > TargetEngineSchemaVersion)
			throw SceneDocumentSerializationException("Scene document uses an unsupported future format or engine schema");
		DocumentMigration Step;
		{
			std::shared_lock Lock(this->Mutex);
			const auto Iterator = this->DocumentMigrations.find({FormatVersion, EngineSchemaVersion});
			if (Iterator == this->DocumentMigrations.end())
				throw SceneDocumentSerializationException("Scene document has no migration path from its current root version");
			Step = Iterator->second;
		}
		if (Step.ToFormatVersion > TargetFormatVersion || Step.ToEngineSchemaVersion > TargetEngineSchemaVersion)
			throw SceneDocumentSerializationException("Scene document migration path overshoots the supported target version");
		try
		{
			Step.Apply(Root);
		}
		catch (const std::exception &Exception)
		{
			throw SceneDocumentSerializationException("Scene document root migration failed: " + string(Exception.what()));
		}
		if (!Root.contains("FormatVersion") || !Root.at("FormatVersion").is_number_unsigned() ||
			Root.at("FormatVersion").get<uint32>() != Step.ToFormatVersion || !Root.contains("EngineSchemaVersion") ||
			!Root.at("EngineSchemaVersion").is_number_unsigned() ||
			Root.at("EngineSchemaVersion").get<uint32>() != Step.ToEngineSchemaVersion)
		{
			throw SceneDocumentSerializationException("Scene document root migration did not publish its declared target version");
		}
		FormatVersion = Step.ToFormatVersion;
		EngineSchemaVersion = Step.ToEngineSchemaVersion;
		ValidateJsonBudget(Root);
	}

	if (!Root.contains("Objects") || !Root.at("Objects").is_array())
		throw SceneDocumentSerializationException("Scene document cannot migrate components without an object array");
	for (Json &Object : Root.at("Objects"))
	{
		if (!Object.is_object() || !Object.contains("Components") || !Object.at("Components").is_object())
			continue;
		for (auto &[ComponentName, Component] : Object.at("Components").items())
		{
			if (!IsKnownComponent(ComponentName))
				continue;
			if (!Component.is_object() || !Component.contains("SchemaVersion") || !Component.at("SchemaVersion").is_number_unsigned())
				throw SceneDocumentSerializationException("Scene component '" + ComponentName +
														  "' cannot be migrated without a schema version");
			uint32 SchemaVersion = Component.at("SchemaVersion").get<uint32>();
			while (SchemaVersion != TargetComponentSchemaVersion)
			{
				if (SchemaVersion > TargetComponentSchemaVersion)
					throw SceneDocumentSerializationException("Scene component '" + ComponentName + "' uses a future schema");
				ComponentMigration Step;
				{
					std::shared_lock Lock(this->Mutex);
					const auto Iterator = this->ComponentMigrations.find({ComponentName, SchemaVersion});
					if (Iterator == this->ComponentMigrations.end())
						throw SceneDocumentSerializationException("Scene component '" + ComponentName + "' has no migration path");
					Step = Iterator->second;
				}
				if (Step.ToSchemaVersion > TargetComponentSchemaVersion)
					throw SceneDocumentSerializationException("Scene component migration overshoots the supported target schema");
				try
				{
					Step.Apply(Component);
				}
				catch (const std::exception &Exception)
				{
					throw SceneDocumentSerializationException("Scene component '" + ComponentName +
															  "' migration failed: " + string(Exception.what()));
				}
				if (!Component.contains("SchemaVersion") || !Component.at("SchemaVersion").is_number_unsigned() ||
					Component.at("SchemaVersion").get<uint32>() != Step.ToSchemaVersion)
				{
					throw SceneDocumentSerializationException("Scene component migration did not publish its declared target schema");
				}
				SchemaVersion = Step.ToSchemaVersion;
				ValidateJsonBudget(Root);
			}
		}
	}
}

namespace
{
[[nodiscard]] Json InstancePropertyToJson(const instance::InstancePropertyValue &Property)
{
	return std::visit(
		[](const auto &Value) -> Json
		{
			using ValueType = std::decay_t<decltype(Value)>;
			if constexpr (std::same_as<ValueType, bool>)
				return {{"Kind", "Boolean"}, {"Value", Value}};
			else if constexpr (std::same_as<ValueType, int32>)
				return {{"Kind", "SignedInteger32"}, {"Value", Value}};
			else if constexpr (std::same_as<ValueType, uint32>)
				return {{"Kind", "UnsignedInteger32"}, {"Value", Value}};
			else if constexpr (std::same_as<ValueType, int64>)
				return {{"Kind", "SignedInteger"}, {"Value", Value}};
			else if constexpr (std::same_as<ValueType, uint64>)
				return {{"Kind", "UnsignedInteger"}, {"Value", Value}};
			else if constexpr (std::same_as<ValueType, float32>)
				return {{"Kind", "Scalar32"}, {"Value", Value}};
			else if constexpr (std::same_as<ValueType, float64>)
				return {{"Kind", "Scalar"}, {"Value", Value}};
			else if constexpr (std::same_as<ValueType, string>)
				return {{"Kind", "String"}, {"Value", Value}};
			else if constexpr (std::same_as<ValueType, glm::vec2>)
				return {{"Kind", "Vector2"}, {"Value", Json::array({Value.x, Value.y})}};
			else if constexpr (std::same_as<ValueType, glm::vec3>)
				return {{"Kind", "Vector3"}, {"Value", Json::array({Value.x, Value.y, Value.z})}};
			else if constexpr (std::same_as<ValueType, glm::vec4>)
				return {{"Kind", "Vector4"}, {"Value", Json::array({Value.x, Value.y, Value.z, Value.w})}};
			else if constexpr (std::same_as<ValueType, glm::quat>)
				return {{"Kind", "Quaternion"}, {"Value", Json::array({Value.w, Value.x, Value.y, Value.z})}};
			else if constexpr (std::same_as<ValueType, util::UUID>)
				return {{"Kind", "InstanceReference"}, {"Value", Value.IsValid() ? Json(Value.ToString()) : Json(nullptr)}};
			else
				return {{"Kind", "AssetReference"},
						{"Value", {{"ID", Value.ID}, {"Type", static_cast<uint32>(Value.Type)}, {"Path", Value.ProjectRelativePath}}}};
		},
		Property);
}

[[nodiscard]] instance::InstancePropertyValue InstancePropertyFromJson(const Json &Node)
{
	if (!Node.is_object() || !Node.contains("Kind") || !Node.at("Kind").is_string() || !Node.contains("Value"))
		throw SceneDocumentSerializationException("Instance property is malformed");
	const string Kind = Node.at("Kind").get<string>();
	const Json &Value = Node.at("Value");
	if (Kind == "Boolean" && Value.is_boolean())
		return Value.get<bool>();
	if (Kind == "SignedInteger32" && Value.is_number_integer())
		return Value.get<int32>();
	if (Kind == "UnsignedInteger32" && Value.is_number_unsigned())
		return Value.get<uint32>();
	if (Kind == "SignedInteger" && Value.is_number_integer())
		return Value.get<int64>();
	if (Kind == "UnsignedInteger" && Value.is_number_unsigned())
		return Value.get<uint64>();
	if (Kind == "Scalar" && Value.is_number())
		return Value.get<float64>();
	if (Kind == "Scalar32" && Value.is_number())
		return Value.get<float32>();
	if (Kind == "String" && Value.is_string())
		return Value.get<string>();
	const auto RequireVector = [&Value, &Kind](const usize Count)
	{
		if (!Value.is_array() || Value.size() != Count ||
			!std::ranges::all_of(Value, [](const Json &Element) { return Element.is_number(); }))
			throw SceneDocumentSerializationException("Instance " + Kind + " property is malformed");
	};
	if (Kind == "Vector2")
	{
		RequireVector(2);
		return glm::vec2(Value[0].get<float32>(), Value[1].get<float32>());
	}
	if (Kind == "Vector3")
	{
		RequireVector(3);
		return glm::vec3(Value[0].get<float32>(), Value[1].get<float32>(), Value[2].get<float32>());
	}
	if (Kind == "Vector4")
	{
		RequireVector(4);
		return glm::vec4(Value[0].get<float32>(), Value[1].get<float32>(), Value[2].get<float32>(), Value[3].get<float32>());
	}
	if (Kind == "Quaternion")
	{
		RequireVector(4);
		return glm::quat(Value[0].get<float32>(), Value[1].get<float32>(), Value[2].get<float32>(), Value[3].get<float32>());
	}
	if (Kind == "InstanceReference" && Value.is_null())
		return util::UUID{};
	if (Kind == "InstanceReference" && Value.is_string())
		return util::UUID::Parse(Value.get<string>());
	if (Kind == "AssetReference" && Value.is_object() && Value.contains("ID") && Value.at("ID").is_string() && Value.contains("Type") &&
		Value.at("Type").is_number_unsigned() && Value.contains("Path") && Value.at("Path").is_string())
	{
		const auto Type = static_cast<resource::AssetType>(Value.at("Type").get<uint32>());
		if (!IsConcreteAssetType(Type) && Type != resource::AssetType::Count)
			throw SceneDocumentSerializationException("Instance asset reference type is invalid");
		return instance::InstanceAssetReference{
			.ID = Value.at("ID").get<string>(), .Type = Type, .ProjectRelativePath = Value.at("Path").get<string>()};
	}
	throw SceneDocumentSerializationException("Instance property kind or value is invalid");
}

[[nodiscard]] Json SerializeInstances(const instance::InstanceGraphSnapshot &Snapshot)
{
	Json Result = Json::array();
	for (const instance::InstanceRecord &Record : Snapshot.Instances)
	{
		Json Properties = Json::object();
		for (const auto &[Name, Value] : Record.Properties)
			Properties[Name] = InstancePropertyToJson(Value);
		Result.push_back({{"ID", Record.ID.ToString()},
						  {"ClassID", Record.ClassID.ToString()},
						  {"ClassName", Record.ClassName},
						  {"Name", Record.Name},
						  {"Parent", Record.Parent.IsValid() ? Json(Record.Parent.ToString()) : Json(nullptr)},
						  {"SiblingOrder", Record.SiblingOrder},
						  {"Enabled", Record.Enabled},
						  {"Protected", Record.Protected},
						  {"Properties", std::move(Properties)}});
	}
	return Result;
}

void MigratePreInstanceDocument(Json &Root)
{
	if (!Root.is_object() || Root.value("FormatVersion", uint32{0}) != 1U ||
		Root.value("EngineSchemaVersion", uint32{0}) != SceneDocumentSerializer::CurrentEngineSchemaVersion || !Root.contains("Objects") ||
		!Root.at("Objects").is_array())
	{
		throw SceneDocumentSerializationException("Scene document has no built-in migration path from its current root version");
	}
	instance::InstanceTypeRegistry Types;
	instance::InstanceGraph Services(Types);
	instance::InstanceGraphSnapshot Snapshot = Services.Snapshot();
	Snapshot.Revision = 1U;
	const auto ReadVector3 = [](const Json &Properties, const string_view Name, const glm::vec3 Fallback)
	{
		const auto Found = Properties.find(string(Name));
		if (Found == Properties.end() || !Found->is_array() || Found->size() != 3U ||
			!std::ranges::all_of(*Found, [](const Json &Value) { return Value.is_number(); }))
		{
			return Fallback;
		}
		return glm::vec3((*Found)[0].get<float32>(), (*Found)[1].get<float32>(), (*Found)[2].get<float32>());
	};
	for (const Json &Object : Root.at("Objects"))
	{
		if (!Object.is_object() || !Object.contains("ID") || !Object.at("ID").is_string() || !Object.contains("Name") ||
			!Object.at("Name").is_string() || !Object.contains("Components") || !Object.at("Components").is_object())
		{
			throw SceneDocumentSerializationException("Pre-instance scene contains an object that cannot be migrated");
		}
		const Json &Components = Object.at("Components");
		instance::InstanceClassID ClassID = instance::class_ids::Model;
		if (Components.contains(string(components::CObjectCameraComponent::ComponentName)))
			ClassID = instance::class_ids::Camera;
		else if (Components.contains(string(components::CObjectDirectionalLightComponent::ComponentName)))
			ClassID = instance::class_ids::DirectionalLight;
		else if (Components.contains(string(components::CObjectPointLightComponent::ComponentName)))
			ClassID = instance::class_ids::PointLight;
		else if (Components.contains(string(components::CObjectSpotLightComponent::ComponentName)))
			ClassID = instance::class_ids::SpotLight;
		else if (Components.contains(string(components::CObjectMeshComponent::ComponentName)))
			ClassID = instance::class_ids::MeshPart;
		const std::shared_ptr<const instance::InstanceTypeDescriptor> Descriptor = Types.Find(ClassID);
		if (Descriptor == nullptr)
			throw SceneDocumentSerializationException("Pre-instance scene maps to an unregistered instance class");
		instance::InstanceRecord Record{.ID = util::UUID::Parse(Object.at("ID").get<string>()),
										.ClassID = ClassID,
										.ClassName = Descriptor->ClassName,
										.Name = Object.at("Name").get<string>(),
										.Properties = Descriptor->DefaultProperties,
										.SiblingOrder = Object.value("SiblingOrder", uint32{0})};
		if (!Record.ID.IsValid())
			throw SceneDocumentSerializationException("Pre-instance scene contains an invalid object identity");
		if (Object.contains("Parent") && Object.at("Parent").is_string())
			Record.Parent = util::UUID::Parse(Object.at("Parent").get<string>());
		else
			Record.Parent = Services.GetWorkspace();

		const auto TransformNode = Components.find(string(components::CObjectTransformComponent::ComponentName));
		if (TransformNode != Components.end() && TransformNode->is_object() && TransformNode->contains("Properties") &&
			TransformNode->at("Properties").is_object())
		{
			const Json &Properties = TransformNode->at("Properties");
			const glm::vec3 Position = ReadVector3(Properties, "Position", glm::vec3(0.0F));
			const glm::vec3 RotationEuler = ReadVector3(Properties, "RotationEuler", glm::vec3(0.0F));
			const glm::quat Rotation = glm::quat(glm::radians(RotationEuler));
			const glm::vec3 Scale = ReadVector3(Properties, "Scale", glm::vec3(1.0F));
			if (ClassID == instance::class_ids::Model)
			{
				Record.Properties.insert_or_assign("PivotPosition", Position);
				Record.Properties.insert_or_assign("PivotRotation", Rotation);
				Record.Properties.insert_or_assign("PivotScale", Scale);
			}
			else
			{
				if (Record.Properties.contains("Position"))
					Record.Properties.insert_or_assign("Position", Position);
				if (Record.Properties.contains("Rotation"))
					Record.Properties.insert_or_assign("Rotation", Rotation);
				if (Record.Properties.contains("Scale"))
					Record.Properties.insert_or_assign("Scale", Scale);
			}
		}
		const auto CopyNumber =
			[&Components, &Record](const string_view ComponentName, const string_view SourceName, const string_view DestinationName)
		{
			const auto Component = Components.find(string(ComponentName));
			if (Component == Components.end() || !Component->is_object() || !Component->contains("Properties"))
				return;
			const Json &Properties = Component->at("Properties");
			const auto Value = Properties.find(string(SourceName));
			if (Value != Properties.end() && Value->is_number() && Record.Properties.contains(string(DestinationName)))
				Record.Properties.insert_or_assign(string(DestinationName), Value->get<float64>());
		};
		const auto CopyBoolean =
			[&Components, &Record](const string_view ComponentName, const string_view SourceName, const string_view DestinationName)
		{
			const auto Component = Components.find(string(ComponentName));
			if (Component == Components.end() || !Component->is_object() || !Component->contains("Properties"))
				return;
			const Json &Properties = Component->at("Properties");
			const auto Value = Properties.find(string(SourceName));
			if (Value != Properties.end() && Value->is_boolean() && Record.Properties.contains(string(DestinationName)))
				Record.Properties.insert_or_assign(string(DestinationName), Value->get<bool>());
		};
		if (ClassID == instance::class_ids::Camera)
		{
			const string_view Component = components::CObjectCameraComponent::ComponentName;
			CopyNumber(Component, "VerticalFieldOfViewDegrees", "FieldOfView");
			CopyNumber(Component, "OrthographicHeight", "OrthographicHeight");
			CopyNumber(Component, "NearPlane", "NearPlane");
			CopyNumber(Component, "FarPlane", "FarPlane");
			CopyNumber(Component, "ExposureCompensation", "ExposureCompensation");
			CopyBoolean(Component, "Primary", "Primary");
			CopyBoolean(Component, "TemporalJitterEnabled", "TemporalJitter");
			const Json &Properties = Components.at(string(Component)).at("Properties");
			Record.Properties.insert_or_assign("Projection", Properties.value("Projection", uint32{0}) == 0U ? string("Perspective")
																											 : string("Orthographic"));
		}
		const auto MigrateLight = [&Components, &Record, &CopyNumber, &CopyBoolean, &ReadVector3](const string_view Component)
		{
			const Json &Properties = Components.at(string(Component)).at("Properties");
			Record.Properties.insert_or_assign("Color", ReadVector3(Properties, "Color", glm::vec3(1.0F)));
			for (const string_view Name : {string_view("ShadowConstantBias"), string_view("ShadowSlopeBias"),
										   string_view("ShadowNormalBias"), string_view("ShadowFilterRadius")})
				CopyNumber(Component, Name, Name);
			CopyBoolean(Component, "CastShadows", "CastShadows");
			const auto Resolution = Properties.find("ShadowResolution");
			if (Resolution != Properties.end() && Resolution->is_number_unsigned())
				Record.Properties.insert_or_assign("ShadowResolution", Resolution->get<uint64>());
		};
		if (ClassID == instance::class_ids::DirectionalLight)
		{
			const string_view Component = components::CObjectDirectionalLightComponent::ComponentName;
			MigrateLight(Component);
			CopyNumber(Component, "IlluminanceLux", "IlluminanceLux");
			CopyNumber(Component, "AngularDiameterDegrees", "AngularDiameterDegrees");
			const Json &Properties = Components.at(string(Component)).at("Properties");
			if (const auto Count = Properties.find("CascadeCount"); Count != Properties.end() && Count->is_number_unsigned())
				Record.Properties.insert_or_assign("CascadeCount", Count->get<uint64>());
			CopyNumber(Component, "CascadeDistributionExponent", "CascadeDistributionExponent");
		}
		else if (ClassID == instance::class_ids::PointLight)
		{
			const string_view Component = components::CObjectPointLightComponent::ComponentName;
			MigrateLight(Component);
			CopyNumber(Component, "LuminousPowerLumens", "LuminousPowerLumens");
			CopyNumber(Component, "Range", "Range");
			CopyNumber(Component, "SourceRadius", "SourceRadius");
		}
		else if (ClassID == instance::class_ids::SpotLight)
		{
			const string_view Component = components::CObjectSpotLightComponent::ComponentName;
			MigrateLight(Component);
			CopyNumber(Component, "LuminousPowerLumens", "LuminousPowerLumens");
			CopyNumber(Component, "Range", "Range");
			CopyNumber(Component, "InnerConeDegrees", "InnerConeDegrees");
			CopyNumber(Component, "OuterConeDegrees", "OuterConeDegrees");
		}
		else if (ClassID == instance::class_ids::MeshPart)
		{
			const Json &Model = Components.at(string(components::CObjectMeshComponent::ComponentName)).at("Properties").at("Model");
			if (Model.is_object())
			{
				Record.Properties.insert_or_assign(
					"Model", instance::InstanceAssetReference{.ID = Model.value("ID", string{}),
															  .Type = static_cast<resource::AssetType>(Model.value("Type", uint32{3})),
															  .ProjectRelativePath = Model.value("Path", string{})});
			}
		}
		Snapshot.Instances.push_back(std::move(Record));
	}
	Root["Instances"] = SerializeInstances(Snapshot);
	Root["FormatVersion"] = SceneDocumentSerializer::CurrentFormatVersion;
}

void MigrateWorkspaceLights(Json &Root)
{
	if (!Root.is_object() || Root.value("FormatVersion", uint32{0}) != 2U ||
		Root.value("EngineSchemaVersion", uint32{0}) != SceneDocumentSerializer::CurrentEngineSchemaVersion ||
		!Root.contains("Instances") || !Root.at("Instances").is_array() || !Root.contains("Objects") || !Root.at("Objects").is_array())
	{
		throw SceneDocumentSerializationException("Scene document has no built-in Workspace-light migration path");
	}

	const string WorkspaceClassID = instance::class_ids::Workspace.ToString();
	const string DirectionalClassID = instance::class_ids::DirectionalLight.ToString();
	const string PointClassID = instance::class_ids::PointLight.ToString();
	const string SpotClassID = instance::class_ids::SpotLight.ToString();
	string WorkspaceID;
	for (const Json &Node : Root.at("Instances"))
	{
		if (Node.is_object() && Node.value("ClassID", string{}) == WorkspaceClassID && Node.contains("ID") && Node.at("ID").is_string())
		{
			WorkspaceID = Node.at("ID").get<string>();
			break;
		}
	}
	if (WorkspaceID.empty())
		throw SceneDocumentSerializationException("Scene document Workspace-light migration cannot find Workspace");

	std::unordered_map<string, const Json *> ObjectTransforms;
	for (const Json &Object : Root.at("Objects"))
	{
		if (!Object.is_object() || !Object.contains("ID") || !Object.at("ID").is_string() || !Object.contains("Components") ||
			!Object.at("Components").is_object())
		{
			continue;
		}
		const Json &Components = Object.at("Components");
		const auto Transform = Components.find(string(components::CObjectTransformComponent::ComponentName));
		if (Transform != Components.end() && Transform->is_object() && Transform->contains("Properties") &&
			Transform->at("Properties").is_object())
		{
			ObjectTransforms.emplace(Object.at("ID").get<string>(), &Transform->at("Properties"));
		}
	}

	const auto ReadVector3 = [](const Json &Properties, const string_view Name, const glm::vec3 Fallback)
	{
		const auto Found = Properties.find(string(Name));
		if (Found == Properties.end() || !Found->is_array() || Found->size() != 3U ||
			!std::ranges::all_of(*Found, [](const Json &Value) { return Value.is_number(); }))
		{
			return Fallback;
		}
		return glm::vec3((*Found)[0].get<float32>(), (*Found)[1].get<float32>(), (*Found)[2].get<float32>());
	};

	for (Json &Node : Root.at("Instances"))
	{
		if (!Node.is_object() || !Node.contains("ClassID") || !Node.at("ClassID").is_string() || !Node.contains("ID") ||
			!Node.at("ID").is_string() || !Node.contains("Properties") || !Node.at("Properties").is_object())
		{
			continue;
		}
		const string ClassID = Node.at("ClassID").get<string>();
		if (ClassID == DirectionalClassID)
			Node["Parent"] = WorkspaceID;
		if (ClassID != PointClassID && ClassID != SpotClassID)
			continue;

		Json &Properties = Node.at("Properties");
		const auto ObjectTransform = ObjectTransforms.find(Node.at("ID").get<string>());
		const Json *Transform = ObjectTransform == ObjectTransforms.end() ? nullptr : ObjectTransform->second;
		if (!Properties.contains("Position"))
		{
			const glm::vec3 Position = Transform == nullptr ? glm::vec3(0.0F) : ReadVector3(*Transform, "Position", glm::vec3(0.0F));
			Properties["Position"] = InstancePropertyToJson(Position);
		}
		if (ClassID == SpotClassID && !Properties.contains("Rotation"))
		{
			const glm::vec3 Euler = Transform == nullptr ? glm::vec3(0.0F) : ReadVector3(*Transform, "RotationEuler", glm::vec3(0.0F));
			Properties["Rotation"] = InstancePropertyToJson(glm::quat(glm::radians(Euler)));
		}
	}
	Root["FormatVersion"] = SceneDocumentSerializer::CurrentFormatVersion;
}

[[nodiscard]] instance::InstanceGraphSnapshot DeserializeInstances(const Json &Root)
{
	if (!Root.contains("Instances") || !Root.at("Instances").is_array() || Root.at("Instances").size() > MaximumObjectCount)
		throw SceneDocumentSerializationException("Scene document instance array is invalid");
	instance::InstanceGraphSnapshot Result{.Revision = 1};
	Result.Instances.reserve(Root.at("Instances").size());
	for (const Json &Node : Root.at("Instances"))
	{
		if (!Node.is_object() || !Node.contains("ID") || !Node.at("ID").is_string() || !Node.contains("ClassID") ||
			!Node.at("ClassID").is_string() || !Node.contains("ClassName") || !Node.at("ClassName").is_string() || !Node.contains("Name") ||
			!Node.at("Name").is_string() || !Node.contains("Parent") || (!Node.at("Parent").is_null() && !Node.at("Parent").is_string()) ||
			!Node.contains("SiblingOrder") || !Node.at("SiblingOrder").is_number_unsigned() || !Node.contains("Enabled") ||
			!Node.at("Enabled").is_boolean() || !Node.contains("Protected") || !Node.at("Protected").is_boolean() ||
			!Node.contains("Properties") || !Node.at("Properties").is_object())
		{
			throw SceneDocumentSerializationException("Scene document contains an invalid instance record");
		}
		instance::InstancePropertyMap Properties;
		for (const auto &[Name, Value] : Node.at("Properties").items())
			Properties.emplace(Name, InstancePropertyFromJson(Value));
		Result.Instances.push_back(
			{.ID = util::UUID::Parse(Node.at("ID").get<string>()),
			 .ClassID = util::UUID::Parse(Node.at("ClassID").get<string>()),
			 .ClassName = Node.at("ClassName").get<string>(),
			 .Name = Node.at("Name").get<string>(),
			 .Parent = Node.at("Parent").is_null() ? util::UUID{} : util::UUID::Parse(Node.at("Parent").get<string>()),
			 .Properties = std::move(Properties),
			 .SiblingOrder = Node.at("SiblingOrder").get<uint32>(),
			 .Enabled = Node.at("Enabled").get<bool>(),
			 .Protected = Node.at("Protected").get<bool>()});
	}
	return Result;
}

[[nodiscard]] Json SerializeScene(const util::UUID &DocumentID, const string_view DocumentName, const world::Scene &Scene,
								  const instance::InstanceGraphSnapshot &Instances, const reflection::ReflectionRegistry &Reflection,
								  resource::AssetManager &Assets)
{
	Json Root{{"FormatVersion", SceneDocumentSerializer::CurrentFormatVersion},
			  {"EngineSchemaVersion", SceneDocumentSerializer::CurrentEngineSchemaVersion},
			  {"MigrationData", Json::object()},
			  {"ID", DocumentID.ToString()},
			  {"Name", DocumentName},
			  {"Instances", SerializeInstances(Instances)},
			  {"Objects", Json::array()}};
	const world::Scene::ReadAccess Access = Scene.Read();
	for (const world::ObjectHandle Object : Access.Objects())
	{
		const auto IdentityHandle = Access.GetComponent<components::CObjectIdentityComponent>(Object);
		const auto HierarchyHandle = Access.GetComponent<components::CObjectHierarchyComponent>(Object);
		util::UUID ID;
		string Name;
		util::UUID ParentID;
		uint32 SiblingOrder = 0;
		const components::CObjectIdentityComponent &Identity = Access.Resolve(IdentityHandle);
		const components::CObjectHierarchyComponent &Hierarchy = Access.Resolve(HierarchyHandle);
		ID = Identity.GetPersistentID();
		Name = Identity.GetName();
		SiblingOrder = Hierarchy.GetSiblingOrder();
		if (Hierarchy.GetParent().IsValid())
		{
			const auto ParentIdentity = Access.GetComponent<components::CObjectIdentityComponent>(Hierarchy.GetParent());
			ParentID = Access.Resolve(ParentIdentity).GetPersistentID();
		}
		Json Components = Json::object();
		SaveComponent<components::CObjectIdentityComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectTransformComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectHierarchyComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectCameraComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectPointLightComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectSpotLightComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectDirectionalLightComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectMeshComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectAnimationComponent>(Components, Access, Object, Reflection, Assets);
		SaveComponent<components::CObjectBehaviorComponent>(Components, Access, Object, Reflection, Assets);

		if (const auto Handle = Access.GetComponent<components::CObjectMeshComponent>(Object); Handle.IsValid())
		{
			Json Overrides = Json::array();
			for (const components::MeshMaterialOverride &Override : Access.Resolve(Handle).GetMaterialOverrides())
			{
				const resource::AssetRecordHandle Record = Assets.GetRecord(Override.Material.GetID());
				if (Record == nullptr)
					throw SceneDocumentSerializationException("Material override references an unknown asset record");
				Overrides.push_back({{"MeshInstance", Override.MeshInstance},
									 {"MaterialSlot", Override.MaterialSlot},
									 {"ID", Override.Material.GetID()},
									 {"Type", static_cast<uint32>(Record->GetType())},
									 {"Path", AssetPath(Assets, Override.Material.GetID()).generic_string()}});
			}
			Components[string(components::CObjectMeshComponent::ComponentName)]["MaterialOverrides"] = std::move(Overrides);
		}
		if (const auto Handle = Access.GetComponent<components::CObjectAnimationComponent>(Object); Handle.IsValid())
		{
			const components::CObjectAnimationComponent &Animation = Access.Resolve(Handle);
			Json Parameters = Json::array();
			for (const components::AnimationParameterValue &Parameter : Animation.GetParameters())
				Parameters.push_back(
					{{"ID", Parameter.ID}, {"Type", static_cast<uint32>(Parameter.Type)}, {"Value", VectorJson(Parameter.Value)}});
			Json MorphWeights = Json::array();
			for (const components::AnimationMorphWeight &Morph : Animation.GetMorphWeights())
				MorphWeights.push_back({{"MorphSet", Morph.MorphSet}, {"Target", Morph.Target}, {"Weight", Morph.Weight}});
			Json RetargetProfiles = Json::array();
			for (const auto &Profile : Animation.GetRetargetProfiles())
				RetargetProfiles.push_back({{"ID", Profile.GetID()},
											{"Type", static_cast<uint32>(resource::AssetType::RetargetProfile)},
											{"Path", AssetPath(Assets, Profile.GetID()).generic_string()}});
			Json &AnimationNode = Components[string(components::CObjectAnimationComponent::ComponentName)];
			AnimationNode["Parameters"] = std::move(Parameters);
			AnimationNode["MorphWeights"] = std::move(MorphWeights);
			AnimationNode["RetargetProfiles"] = std::move(RetargetProfiles);
		}
		if (const auto Handle = Access.GetComponent<components::CObjectBehaviorComponent>(Object); Handle.IsValid())
		{
			Json Behaviors = Json::array();
			for (const components::BehaviorInstance &Behavior : Access.Resolve(Handle).GetBehaviors())
			{
				Json Properties = Json::object();
				for (const auto &[PropertyName, Value] : Behavior.Properties)
					Properties[PropertyName] = BehaviorValueToJson(Value);
				Behaviors.push_back({{"InstanceID", Behavior.InstanceID.ToString()},
									 {"Type", Behavior.Type},
									 {"TypeName", Behavior.TypeName},
									 {"ModuleName", Behavior.ModuleName},
									 {"StableTypeID", Behavior.StableTypeID.ToString()},
									 {"SchemaVersion", Behavior.SchemaVersion},
									 {"Enabled", Behavior.Enabled},
									 {"Properties", std::move(Properties)}});
			}
			Components[string(components::CObjectBehaviorComponent::ComponentName)]["Behaviors"] = std::move(Behaviors);
		}
		Root["Objects"].push_back({{"ID", ID.ToString()},
								   {"Name", Name},
								   {"Parent", ParentID.IsValid() ? Json(ParentID.ToString()) : Json(nullptr)},
								   {"SiblingOrder", SiblingOrder},
								   {"Components", std::move(Components)}});
	}
	auto &Objects = Root["Objects"].get_ref<Json::array_t &>();
	std::ranges::sort(Objects, [](const Json &Left, const Json &Right)
					  { return Left.at("ID").get_ref<const string &>() < Right.at("ID").get_ref<const string &>(); });
	return Root;
}

void WriteScene(const Json &Root, const std::filesystem::path &Destination, const document::SceneDocument *SourceDocument = nullptr,
				const uint64 ExpectedRevision = 0)
{
	if (Destination.empty())
		throw SceneDocumentSerializationException("Scene save requires an explicit destination");
	const std::filesystem::path AbsoluteDestination = std::filesystem::absolute(Destination).lexically_normal();
	core::io::SecurePath::CreateTrustedRoot(AbsoluteDestination.parent_path(), "Scene document root");
	const std::filesystem::path Temporary = AbsoluteDestination.filename().wstring() + L"." +
											std::filesystem::path(util::UUID::GenerateRandomUUID().ToString()).wstring() + L".tmp";
	const string Serialized = Root.dump(2) + '\n';
	if (Serialized.size() > MaximumDocumentBytes)
		throw SceneDocumentSerializationException("Serialized scene document exceeds the supported file-size budget");
	core::io::SecurePath::WriteFileWithin(AbsoluteDestination.parent_path(), Temporary,
										  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), false, true,
										  "Scene document temporary file");
	try
	{
		const Json RoundTrip = ReadJsonFile(AbsoluteDestination.parent_path() / Temporary);
		ValidateSerializedRoot(RoundTrip);
	}
	catch (...)
	{
		core::io::SecurePath::RemoveWithin(AbsoluteDestination.parent_path(), Temporary, false, "Invalid scene document temporary file");
		throw;
	}
	if (SourceDocument != nullptr && SourceDocument->GetRevision() != ExpectedRevision)
	{
		core::io::SecurePath::RemoveWithin(AbsoluteDestination.parent_path(), Temporary, false, "Stale scene document temporary file");
		throw SceneDocumentSerializationException("Scene document changed while its save snapshot was being written");
	}
	core::io::SecurePath::ReplaceWithin(AbsoluteDestination.parent_path(), Temporary, AbsoluteDestination.filename(),
										"Scene document publication");
}
} // namespace

void SceneDocumentSerializer::Save(document::SceneDocument &Document, const reflection::ReflectionRegistry &Reflection,
								   resource::AssetManager &Assets, const std::filesystem::path &Path)
{
	Document.AssertOwnerThread();
	const std::filesystem::path Destination = Path.empty() ? Document.GetPath() : Path;
	const uint64 Revision = Document.GetRevision();
	Json Root =
		SerializeScene(Document.GetID(), Document.GetName(), Document.GetScene(), Document.GetInstances().Snapshot(), Reflection, Assets);
	Root = MergePreservedData(std::move(Root), Document.GetPreservedSerializationData());
	WriteScene(Root, Destination, &Document, Revision);
	Document.SetPreservedSerializationData(Root.dump());
	Document.MarkSaved(Destination);
}

void SceneDocumentSerializer::SaveSnapshot(const util::UUID &DocumentID, const string_view DocumentName, const world::Scene &Scene,
										   const instance::InstanceGraphSnapshot &Instances,
										   const reflection::ReflectionRegistry &Reflection, resource::AssetManager &Assets,
										   const std::filesystem::path &Path, const uint64 Revision, const int64 TimestampMilliseconds)
{
	if (!DocumentID.IsValid() || DocumentName.empty() || Revision == 0 || TimestampMilliseconds <= 0)
		throw SceneDocumentSerializationException("Scene snapshot requires a valid document identity and name");
	Json Root = SerializeScene(DocumentID, DocumentName, Scene, Instances, Reflection, Assets);
	const string CanonicalContent = Root.dump();
	const uint64 ContentChecksum = core::io::CompressedArchive::CalculateChecksum(
		std::span(reinterpret_cast<const uint8 *>(CanonicalContent.data()), CanonicalContent.size()));
	Root["Recovery"] = {{"DocumentID", DocumentID.ToString()},
						{"Revision", Revision},
						{"TimestampMilliseconds", TimestampMilliseconds},
						{"ContentChecksum", ContentChecksum}};
	WriteScene(Root, Path);
}

std::unique_ptr<document::SceneDocument> SceneDocumentSerializer::Load(const std::filesystem::path &Path,
																	   const reflection::ReflectionRegistry &Reflection,
																	   resource::AssetManager &Assets, const usize CommandHistoryCapacity,
																	   const SceneDocumentMigrationRegistry *Migrations)
{
	Json Root = ReadJsonFile(Path);
	if (Root.is_object() && Root.value("FormatVersion", uint32{0}) == 1U)
		MigratePreInstanceDocument(Root);
	else if (Root.is_object() && Root.value("FormatVersion", uint32{0}) == 2U)
		MigrateWorkspaceLights(Root);
	if (Migrations != nullptr)
	{
		Migrations->Migrate(Root, SceneDocumentSerializer::CurrentFormatVersion, SceneDocumentSerializer::CurrentEngineSchemaVersion,
							SceneDocumentSerializer::CurrentComponentSchemaVersion);
	}
	ValidateSerializedRoot(Root);
	const instance::InstanceGraphSnapshot InstanceSnapshot = DeserializeInstances(Root);
	PreflightReflectedProperties(Root, Reflection, Assets);
	resource::AssetLoadTransaction AssetTransaction = Assets.BeginLoadTransaction();
	auto Document = std::make_unique<document::SceneDocument>(Root.at("Name").get<string>(), world::SceneCapacitySpecification{},
															  util::UUID::Parse(Root.at("ID").get<string>()), CommandHistoryCapacity);
	try
	{
		Document->GetInstances().LoadSnapshot(InstanceSnapshot);
	}
	catch (const std::exception &Exception)
	{
		throw SceneDocumentSerializationException("Scene instance graph is invalid: " + string(Exception.what()));
	}
	std::unordered_map<util::UUID, world::ObjectHandle> Objects;
	for (const Json &ObjectNode : Root["Objects"])
	{
		const util::UUID ID = util::UUID::Parse(ObjectNode.at("ID").get<string>());
		const world::ObjectHandle Object = Document->CreateObject(ObjectNode.at("Name").get<string>(), {}, ID);
		Objects.emplace(ID, Object);
	}
	for (const Json &ObjectNode : Root["Objects"])
	{
		const util::UUID ID = util::UUID::Parse(ObjectNode.at("ID").get<string>());
		const world::ObjectHandle Object = Objects.at(ID);
		const Json &Components = ObjectNode.at("Components");
		const auto Has = [&Components](const string_view Name) { return Components.contains(string(Name)); };
		if (Has(components::CObjectCameraComponent::ComponentName))
			(void)Document->GetScene().AddComponent<components::CObjectCameraComponent>(Object);
		if (Has(components::CObjectPointLightComponent::ComponentName))
			(void)Document->GetScene().AddComponent<components::CObjectPointLightComponent>(Object);
		if (Has(components::CObjectSpotLightComponent::ComponentName))
			(void)Document->GetScene().AddComponent<components::CObjectSpotLightComponent>(Object);
		if (Has(components::CObjectDirectionalLightComponent::ComponentName))
			(void)Document->GetScene().AddComponent<components::CObjectDirectionalLightComponent>(Object);
		if (Has(components::CObjectMeshComponent::ComponentName))
		{
			const Json &Reference = Components.at(string(components::CObjectMeshComponent::ComponentName)).at("Properties").at("Model");
			(void)Document->GetScene().AddComponent<components::CObjectMeshComponent>(
				Object, LoadAssetReference<resource::ModelAsset>(Reference, Assets, "Mesh.Model"));
		}
		if (Has(components::CObjectAnimationComponent::ComponentName))
		{
			const Json &Reference =
				Components.at(string(components::CObjectAnimationComponent::ComponentName)).at("Properties").at("Graph");
			(void)Document->GetScene().AddComponent<components::CObjectAnimationComponent>(
				Object, LoadAssetReference<resource::AnimationGraphAsset>(Reference, Assets, "Animation.Graph"));
		}
		if (Has(components::CObjectBehaviorComponent::ComponentName))
			(void)Document->GetScene().AddComponent<components::CObjectBehaviorComponent>(Object);

		ApplyComponentProperties<components::CObjectIdentityComponent>(
			*Document, Object, Components.at(string(components::CObjectIdentityComponent::ComponentName)), Reflection, Assets, Objects);
		ApplyComponentProperties<components::CObjectTransformComponent>(
			*Document, Object, Components.at(string(components::CObjectTransformComponent::ComponentName)), Reflection, Assets, Objects);
		ApplyComponentProperties<components::CObjectHierarchyComponent>(
			*Document, Object, Components.at(string(components::CObjectHierarchyComponent::ComponentName)), Reflection, Assets, Objects);
#define APPLY_OPTIONAL_COMPONENT(ComponentType)                                                                                            \
	if (Has(ComponentType::ComponentName))                                                                                                 \
	ApplyComponentProperties<ComponentType>(*Document, Object, Components.at(string(ComponentType::ComponentName)), Reflection, Assets,    \
											Objects)
		APPLY_OPTIONAL_COMPONENT(components::CObjectCameraComponent);
		APPLY_OPTIONAL_COMPONENT(components::CObjectPointLightComponent);
		APPLY_OPTIONAL_COMPONENT(components::CObjectSpotLightComponent);
		APPLY_OPTIONAL_COMPONENT(components::CObjectDirectionalLightComponent);
		APPLY_OPTIONAL_COMPONENT(components::CObjectMeshComponent);
		APPLY_OPTIONAL_COMPONENT(components::CObjectAnimationComponent);
		APPLY_OPTIONAL_COMPONENT(components::CObjectBehaviorComponent);
#undef APPLY_OPTIONAL_COMPONENT

		if (Has(components::CObjectMeshComponent::ComponentName))
		{
			const Json &Node = Components.at(string(components::CObjectMeshComponent::ComponentName));
			const auto Handle = Document->GetScene().GetComponent<components::CObjectMeshComponent>(Object);
			auto Access = Document->GetScene().Write();
			for (const Json &Override : Node.value("MaterialOverrides", Json::array()))
			{
				const auto Type = static_cast<resource::AssetType>(Override.at("Type").get<uint32>());
				const reflection::AssetReference Reference = ReadAssetReference(Override, Assets, "Mesh.MaterialOverride");
				if (Reference.Type != Type || (Type != resource::AssetType::Material && Type != resource::AssetType::MaterialInstance))
					throw SceneDocumentSerializationException("Mesh material override uses an invalid material asset type");
				auto Material = Assets.GetAsset<resource::MaterialInterfaceAsset>(Type, Override.at("Path").get<string>());
				if (!Material || (!Reference.ID.empty() && Material.GetID() != Reference.ID))
					throw SceneDocumentSerializationException("Mesh material override loaded asset does not match its serialized identity");
				Access.Resolve(Handle).SetMaterialOverride(Override.at("MeshInstance").get<resource::ModelMeshInstanceID>(),
														   Override.at("MaterialSlot").get<resource::MaterialSlotID>(),
														   std::move(Material));
			}
		}
		if (Has(components::CObjectAnimationComponent::ComponentName))
		{
			const Json &Node = Components.at(string(components::CObjectAnimationComponent::ComponentName));
			const auto Handle = Document->GetScene().GetComponent<components::CObjectAnimationComponent>(Object);
			auto Access = Document->GetScene().Write();
			components::CObjectAnimationComponent &Animation = Access.Resolve(Handle);
			for (const Json &Parameter : Node.value("Parameters", Json::array()))
				Animation.SetParameter(Parameter.at("ID").get<resource::AnimationParameterID>(),
									   static_cast<resource::AnimationParameterType>(Parameter.at("Type").get<uint32>()),
									   ReadVector<glm::vec4, 4>(Parameter.at("Value"), "animation parameter"));
			for (const Json &Morph : Node.value("MorphWeights", Json::array()))
				Animation.SetMorphWeight(Morph.value("MorphSet", resource::AssetID{}), Morph.at("Target").get<resource::MorphTargetID>(),
										 Morph.at("Weight").get<float32>());
			for (const Json &Profile : Node.value("RetargetProfiles", Json::array()))
			{
				if (Profile.is_string())
					Animation.SetRetargetProfile(Assets.GetAsset<resource::RetargetProfileAsset>(Profile.get<string>()));
				else
					Animation.SetRetargetProfile(
						LoadAssetReference<resource::RetargetProfileAsset>(Profile, Assets, "Animation.RetargetProfile"));
			}
		}
		if (Has(components::CObjectBehaviorComponent::ComponentName))
		{
			const Json &Node = Components.at(string(components::CObjectBehaviorComponent::ComponentName));
			const auto Handle = Document->GetScene().GetComponent<components::CObjectBehaviorComponent>(Object);
			auto Access = Document->GetScene().Write();
			components::CObjectBehaviorComponent &BehaviorComponent = Access.Resolve(Handle);
			for (const Json &BehaviorNode : Node.value("Behaviors", Json::array()))
			{
				components::BehaviorInstance Behavior{.InstanceID = util::UUID::Parse(BehaviorNode.at("InstanceID").get<string>()),
													  .Type = BehaviorNode.at("Type").get<components::BehaviorTypeID>(),
													  .TypeName = BehaviorNode.at("TypeName").get<string>(),
													  .ModuleName = BehaviorNode.value("ModuleName", string("Engine")),
													  .StableTypeID = BehaviorNode.contains("StableTypeID")
																		  ? util::UUID::Parse(BehaviorNode.at("StableTypeID").get<string>())
																		  : util::UUID{},
													  .SchemaVersion = BehaviorNode.at("SchemaVersion").get<uint32>(),
													  .Enabled = BehaviorNode.value("Enabled", true)};
				for (const auto &[Name, Value] : BehaviorNode.at("Properties").items())
					Behavior.Properties.emplace(Name, BehaviorValueFromJson(Value));
				(void)BehaviorComponent.AddBehavior(std::move(Behavior));
			}
		}
	}
	for (const Json &ObjectNode : Root["Objects"])
	{
		if (ObjectNode.at("Parent").is_null())
			continue;
		Document->SetParent(util::UUID::Parse(ObjectNode.at("ID").get<string>()), util::UUID::Parse(ObjectNode.at("Parent").get<string>()),
							ObjectNode.value("SiblingOrder", uint32{0}));
	}
	Document->GetSelection().Clear();
	Document->SetPreservedSerializationData(Root.dump());
	Document->MarkSaved(std::filesystem::absolute(Path).lexically_normal());
	AssetTransaction.Commit();
	return Document;
}
} // namespace editor::serialization
