#pragma once
#include "src/Types.h"
#include "src/meta.h"

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

template<typename Dependencies, typename Registry>
struct DependenciesRegistered;

template<typename Registry>
struct DependenciesRegistered<TypeList<>, Registry>
{
	static constexpr bool value = true;
};

template<typename Dependency, typename... Rest, typename Registry>
struct DependenciesRegistered<TypeList<Dependency, Rest...>, Registry>
{
	static constexpr bool value = TypeListContains<Dependency, Registry>::value &&
		DependenciesRegistered<TypeList<Rest...>, Registry>::value;
};

template<typename DependencyList, typename Visited>
struct CheckDependencyList;

template<typename T, typename Visited = TypeList<>>
struct CheckNoCyclicDependencies
{
	static_assert(
		!TypeListContains<T, Visited>::value,
		"Circular component dependency detected");

	using WithT = typename Visited::template Append<T>;

	static constexpr bool value =
		CheckDependencyList<GetDependencies<T>, WithT>::value;
};

template<typename Visited>
struct CheckDependencyList<TypeList<>, Visited>
{
	static constexpr bool value = true;
};

template<typename Dependency, typename... Rest, typename Visited>
struct CheckDependencyList<TypeList<Dependency, Rest...>, Visited>
{
	static constexpr bool value =
		CheckNoCyclicDependencies<Dependency, Visited>::value&&
		CheckDependencyList<TypeList<Rest...>, Visited>::value;
};

template<typename List>
struct ValidateComponentDependencies;

template<>
struct ValidateComponentDependencies<TypeList<>>
{
};

template<typename T, typename... Rest>
struct ValidateComponentDependencies<TypeList<T, Rest...>>
	: ValidateComponentDependencies<TypeList<Rest...>>
{
	static constexpr bool value = CheckNoCyclicDependencies<T>::value;
	static_assert(value,
		"Circular dependency detected in component type list");
};

namespace components
{
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

struct _ComponentDependencyValidation
	: ValidateComponentDependencies<components::ComponentTypeList> {
};


//static_assert(TypeListSubtypeOf<components::CObjectComponent, components::ComponentTypeList>::value,
//	"");

//TODO add assert to check right types are in type list