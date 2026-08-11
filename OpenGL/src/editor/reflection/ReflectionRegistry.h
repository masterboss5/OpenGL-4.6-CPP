#pragma once

#include "src/concepts.h"
#include "src/resource/asset/AssetTypes.h"
#include "src/scene/SceneHandles.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm.hpp>
#include <gtc/quaternion.hpp>

namespace resource
{
class AssetManager;
}

namespace world
{
class Scene;
}

namespace editor::reflection
{
using ReflectionTypeID = uint64;

namespace detail
{
template <typename Callable, typename ObjectType, typename ValueType>
concept PropertyReader =
	std::invocable<Callable, const ObjectType &> && std::convertible_to<std::invoke_result_t<Callable, const ObjectType &>, ValueType>;
}

enum class PropertyKind : uint8
{
	Boolean,
	SignedInteger,
	UnsignedInteger,
	Scalar,
	String,
	StringList,
	Vector2,
	Vector3,
	Vector4,
	Quaternion,
	Color,
	UUID,
	ObjectReference,
	AssetReference
};

enum class PropertyFlags : uint32
{
	None = 0,
	ReadOnly = 1U << 0U,
	Hidden = 1U << 1U,
	Advanced = 1U << 2U,
	Multiline = 1U << 3U,
	Angle = 1U << 4U,
	Normalized = 1U << 5U,
	Bitmask = 1U << 6U
};

[[nodiscard]] constexpr PropertyFlags operator|(const PropertyFlags Left, const PropertyFlags Right) noexcept
{
	return static_cast<PropertyFlags>(static_cast<uint32>(Left) | static_cast<uint32>(Right));
}

[[nodiscard]] constexpr bool HasFlag(const PropertyFlags Value, const PropertyFlags Flag) noexcept
{
	return (static_cast<uint32>(Value) & static_cast<uint32>(Flag)) != 0;
}

struct AssetReference final
{
	resource::AssetID ID;
	resource::AssetType Type = resource::AssetType::Count;

	[[nodiscard]] bool operator==(const AssetReference &) const noexcept = default;
};

using PropertyValue = std::variant<bool, int32, uint32, int64, uint64, float32, float64, string, std::vector<string>, glm::vec2, glm::vec3,
								   glm::vec4, glm::quat, util::UUID, world::ObjectHandle, AssetReference>;

struct NumericPropertyMetadata final
{
	std::optional<float64> Minimum;
	std::optional<float64> Maximum;
	std::optional<float64> Step;
};

struct EnumPropertyOption final
{
	uint64 Value = 0;
	string DisplayName;
};

struct PropertyWriteContext final
{
	world::Scene *Scene = nullptr;
	resource::AssetManager *Assets = nullptr;
};

struct PropertyDescriptor final
{
	string Name;
	string DisplayName;
	string Category;
	PropertyKind Kind = PropertyKind::String;
	PropertyFlags Flags = PropertyFlags::None;
	NumericPropertyMetadata Numeric;
	std::vector<EnumPropertyOption> EnumOptions;
	std::optional<PropertyValue> DefaultValue;
	std::function<PropertyValue(const void *)> Read;
	std::function<void(void *, const PropertyValue &, const PropertyWriteContext &)> Write;
};

struct TypeDescriptor final
{
	ReflectionTypeID ID = 0;
	string Name;
	string DisplayName;
	std::vector<PropertyDescriptor> Properties;
};

class ReflectionRegistry final
{
  public:
	void Register(TypeDescriptor Descriptor);
	void Unregister(ReflectionTypeID ID);
	void Clear();
	[[nodiscard]] std::optional<TypeDescriptor> Find(ReflectionTypeID ID) const;
	[[nodiscard]] std::optional<TypeDescriptor> Find(string_view Name) const;
	[[nodiscard]] std::vector<TypeDescriptor> Snapshot() const;
	[[nodiscard]] uint64 GetGeneration() const;

	[[nodiscard]] static ReflectionTypeID MakeTypeID(string_view StableName) noexcept;

  private:
	mutable std::shared_mutex Mutex;
	std::unordered_map<ReflectionTypeID, TypeDescriptor> Types;
	std::unordered_map<string, ReflectionTypeID> Names;
	uint64 Generation = 0;
};

template <typename ObjectType, typename ValueType>
	requires std::constructible_from<PropertyValue, ValueType> && requires(const PropertyValue &Value) { std::get_if<ValueType>(&Value); }
[[nodiscard]] PropertyDescriptor MakeMemberProperty(string Name, string DisplayName, string Category, const PropertyKind Kind,
													ValueType ObjectType::*Member, const PropertyFlags Flags = PropertyFlags::None,
													NumericPropertyMetadata Numeric = {})
{
	if (Member == nullptr)
		throw std::invalid_argument("Reflected member property cannot use a null member pointer");

	PropertyDescriptor Descriptor{.Name = std::move(Name),
								  .DisplayName = std::move(DisplayName),
								  .Category = std::move(Category),
								  .Kind = Kind,
								  .Flags = Flags,
								  .Numeric = Numeric};
	Descriptor.Read = [Member](const void *Object) -> PropertyValue
	{
		if (Object == nullptr)
			throw std::invalid_argument("Cannot read a reflected property from a null object");
		return static_cast<const ObjectType *>(Object)->*Member;
	};
	if (!HasFlag(Flags, PropertyFlags::ReadOnly))
	{
		Descriptor.Write = [Member](void *Object, const PropertyValue &Value, const PropertyWriteContext &)
		{
			if (Object == nullptr)
				throw std::invalid_argument("Cannot write a reflected property to a null object");
			const ValueType *TypedValue = std::get_if<ValueType>(&Value);
			if (TypedValue == nullptr)
				throw std::invalid_argument("Reflected property value has the wrong concrete type");
			static_cast<ObjectType *>(Object)->*Member = *TypedValue;
		};
	}
	if constexpr (std::constructible_from<ObjectType, world::ObjectHandle>)
	{
		const ObjectType DefaultObject(world::ObjectHandle{});
		Descriptor.DefaultValue = DefaultObject.*Member;
	}
	return Descriptor;
}

template <typename ObjectType, typename ValueType, typename Reader, typename Writer>
	requires detail::PropertyReader<Reader, ObjectType, ValueType> &&
			 std::invocable<Writer, ObjectType &, const ValueType &, const PropertyWriteContext &> &&
			 std::constructible_from<PropertyValue, ValueType> && requires(const PropertyValue &Value) { std::get_if<ValueType>(&Value); }
[[nodiscard]] PropertyDescriptor MakeProperty(string Name, string DisplayName, string Category, const PropertyKind Kind, Reader Read,
											  Writer Write, const PropertyFlags Flags = PropertyFlags::None,
											  NumericPropertyMetadata Numeric = {}, std::vector<EnumPropertyOption> EnumOptions = {},
											  std::optional<PropertyValue> DefaultValue = std::nullopt)
{
	if (HasFlag(Flags, PropertyFlags::ReadOnly))
		throw std::invalid_argument("A writable reflected property cannot be declared read-only");

	PropertyDescriptor Descriptor{.Name = std::move(Name),
								  .DisplayName = std::move(DisplayName),
								  .Category = std::move(Category),
								  .Kind = Kind,
								  .Flags = Flags,
								  .Numeric = std::move(Numeric),
								  .EnumOptions = std::move(EnumOptions),
								  .DefaultValue = std::move(DefaultValue)};
	if (!Descriptor.DefaultValue.has_value())
	{
		if constexpr (std::constructible_from<ObjectType, world::ObjectHandle>)
		{
			const ObjectType DefaultObject(world::ObjectHandle{});
			Descriptor.DefaultValue = static_cast<ValueType>(std::invoke(Read, DefaultObject));
		}
	}
	Descriptor.Read = [Read = std::move(Read)](const void *Object) -> PropertyValue
	{
		if (Object == nullptr)
			throw std::invalid_argument("Cannot read a reflected property from a null object");
		return static_cast<ValueType>(std::invoke(Read, *static_cast<const ObjectType *>(Object)));
	};
	Descriptor.Write = [Write = std::move(Write)](void *Object, const PropertyValue &Value, const PropertyWriteContext &Context)
	{
		if (Object == nullptr)
			throw std::invalid_argument("Cannot write a reflected property to a null object");
		const ValueType *TypedValue = std::get_if<ValueType>(&Value);
		if (TypedValue == nullptr)
			throw std::invalid_argument("Reflected property value has the wrong concrete type");
		std::invoke(Write, *static_cast<ObjectType *>(Object), *TypedValue, Context);
	};
	return Descriptor;
}

template <typename ObjectType, typename ValueType, typename Reader>
	requires detail::PropertyReader<Reader, ObjectType, ValueType> && std::constructible_from<PropertyValue, ValueType>
[[nodiscard]] PropertyDescriptor MakeReadOnlyProperty(string Name, string DisplayName, string Category, const PropertyKind Kind,
													  Reader Read, const PropertyFlags AdditionalFlags = PropertyFlags::None,
													  NumericPropertyMetadata Numeric = {},
													  std::vector<EnumPropertyOption> EnumOptions = {})
{
	PropertyDescriptor Descriptor{.Name = std::move(Name),
								  .DisplayName = std::move(DisplayName),
								  .Category = std::move(Category),
								  .Kind = Kind,
								  .Flags = AdditionalFlags | PropertyFlags::ReadOnly,
								  .Numeric = std::move(Numeric),
								  .EnumOptions = std::move(EnumOptions)};
	Descriptor.Read = [Read = std::move(Read)](const void *Object) -> PropertyValue
	{
		if (Object == nullptr)
			throw std::invalid_argument("Cannot read a reflected property from a null object");
		return static_cast<ValueType>(std::invoke(Read, *static_cast<const ObjectType *>(Object)));
	};
	return Descriptor;
}
} // namespace editor::reflection
