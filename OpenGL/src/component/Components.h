#pragma once
#include "src/types.h"
#include "src/meta.h"

namespace components
{

	template<typename T>
	concept HasDependencies = requires
	{
		typename T::Dependencies;
	};

	template<typename T, bool = HasDependencies<T>>
	struct GetDependenciesImpl
	{
		using type = TypeList<>;
	};

	template<typename T>
	struct GetDependenciesImpl<T, true>
	{
		using type = typename T::Dependencies;
	};

	template<typename T>
	using GetDependencies = typename GetDependenciesImpl<T>::type;

	class CObjectComponent;

#define REGISTER_COMPONENT(type) class type;
#include "src/component/components.inl"
#undef REGISTER_COMPONENT

#define REGISTER_COMPONENT(type) ::Append<components::type>
	using ComponentTypeList = TypeList<>
#include "src/component/components.inl"
		;
#undef REGISTER_COMPONENT

	template<typename T>
	inline constexpr uint32 ComponentTypeID =
		static_cast<uint32>(TypeListIndex<T, ComponentTypeList>::value);

	inline constexpr uint32 COBJECT_COMPONENTS =
		ComponentTypeList::size;
}

#define CCOMPONENT_BODY(type) \
	inline static constexpr uint32 TYPE_ID = \
		static_cast<uint32>(ComponentTypeID<type>); \
	std::string_view getComponentName() const override final \
	{ \
		return #type; \
	} \
	\
	virtual uint32 getTypeID() const override final \
	{ \
		return TYPE_ID; \
	} 