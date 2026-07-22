#pragma once
#include "src/meta.h"
#include "src/types.h"

namespace components
{

template <typename T>
concept HasDependencies = requires { typename T::Dependencies; };

template <typename T, bool = HasDependencies<T>> struct GetDependenciesImpl
{
	using Type = TypeList<>;
};

template <typename T> struct GetDependenciesImpl<T, true>
{
	using Type = typename T::Dependencies;
};

template <typename T> using GetDependencies = typename GetDependenciesImpl<T>::Type;

class CObjectComponent;

#define REGISTER_COMPONENT(type) class type;
#include "src/component/components.inl"
#undef REGISTER_COMPONENT

#define REGISTER_COMPONENT(type) ::Append<components::type>
using ComponentTypeList = TypeList<>
#include "src/component/components.inl"
	;
#undef REGISTER_COMPONENT

template <typename T> inline constexpr uint32 ComponentTypeID = static_cast<uint32>(TypeListIndex<T, ComponentTypeList>::Value);

template <typename T> struct ComponentRegistration final : std::bool_constant<TypeListContains<T, ComponentTypeList>::Value>
{
};

inline constexpr uint32 CObjectComponents = ComponentTypeList::Size;
} // namespace components

#define CCOMPONENT_BODY(type)                                                                                                              \
	inline static constexpr uint32 TypeID = static_cast<uint32>(ComponentTypeID<type>);                                                    \
	inline static constexpr string_view ComponentName = #type;                                                                             \
	string_view GetComponentName() const noexcept override final                                                                           \
	{                                                                                                                                      \
		return ComponentName;                                                                                                              \
	}                                                                                                                                      \
                                                                                                                                           \
	uint32 GetTypeID() const noexcept override final                                                                                       \
	{                                                                                                                                      \
		return TypeID;                                                                                                                     \
	}
