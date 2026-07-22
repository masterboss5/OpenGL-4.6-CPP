#pragma once
#include "src/component/Components.h"
#include "src/meta.h"

/*Must include all components*/

#include "src/component/object/CObjectAnimationComponent.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"

namespace component::validation
{
template <typename Dependencies, typename Registry> struct DependenciesRegistered;

template <typename Registry> struct DependenciesRegistered<TypeList<>, Registry>
{
	static constexpr bool Value = true;
};

template <typename Dependency, typename... Rest, typename Registry> struct DependenciesRegistered<TypeList<Dependency, Rest...>, Registry>
{
	static constexpr bool Value =
		TypeListContains<Dependency, Registry>::Value && DependenciesRegistered<TypeList<Rest...>, Registry>::Value;
};

template <typename Components, typename Registry> struct AllDependenciesRegistered;

template <typename Registry> struct AllDependenciesRegistered<TypeList<>, Registry>
{
	static constexpr bool Value = true;
};

template <typename First, typename... Rest, typename Registry> struct AllDependenciesRegistered<TypeList<First, Rest...>, Registry>
{
	static constexpr bool Value = DependenciesRegistered<components::GetDependencies<First>, Registry>::Value &&
								  AllDependenciesRegistered<TypeList<Rest...>, Registry>::Value;
};

template <typename Dependencies, typename Visited> struct IterateDependencies;

template <typename Component, typename Visited, bool AlreadyVisited = TypeListContains<Component, Visited>::Value>
struct NoCyclicDependencies;

template <typename Component, typename Visited> struct NoCyclicDependencies<Component, Visited, true>
{
	static constexpr bool Value = false;
};

template <typename Component, typename Visited> struct NoCyclicDependencies<Component, Visited, false>
{
  private:
	using Next = typename Visited::template Append<Component>;

  public:
	static constexpr bool Value = IterateDependencies<components::GetDependencies<Component>, Next>::Value;
};

template <typename Visited> struct IterateDependencies<TypeList<>, Visited>
{
	static constexpr bool Value = true;
};

template <typename Dependency, typename... Rest, typename Visited> struct IterateDependencies<TypeList<Dependency, Rest...>, Visited>
{
	static constexpr bool Value =
		NoCyclicDependencies<Dependency, Visited>::Value && IterateDependencies<TypeList<Rest...>, Visited>::Value;
};

template <typename Dependencies> struct AllNoCircularDependencies;

template <> struct AllNoCircularDependencies<TypeList<>>
{
	static constexpr bool Value = true;
};

template <typename First, typename... Rest> struct AllNoCircularDependencies<TypeList<First, Rest...>>
{
  private:
	using Next = TypeList<Rest...>;

  public:
	static constexpr bool Value = NoCyclicDependencies<First, TypeList<>>::Value && AllNoCircularDependencies<Next>::Value;
};

namespace object
{
/*No duplicate types*/
static_assert(TypeListUnique<components::ComponentTypeList>::Value, "Duplicate component type");

/*All components must be final*/
static_assert(TypeListAllFinal<components::ComponentTypeList>::Value, "Every registered component must be final");

/*No abstract components*/
static_assert(TypeListAllNotAbstract<components::ComponentTypeList>::Value, "Registered components cannot be abstract");

static_assert(
	[]<typename... ComponentTypes>(TypeList<ComponentTypes...>)
	{
		return (std::is_nothrow_destructible_v<ComponentTypes> && ...) && (std::is_nothrow_move_constructible_v<ComponentTypes> && ...);
	}(components::ComponentTypeList{}),
	"Every registered component must be nothrow destructible and nothrow move constructible");

/*All must be a CObjectComponent*/
static_assert(TypeListAllDerivedFrom<components::CObjectComponent, components::ComponentTypeList>::Value,
			  "Every registered component must derive from CObjectComponent");

/*Every dependency must be registered*/
static_assert(AllDependenciesRegistered<components::ComponentTypeList, components::ComponentTypeList>::Value,
			  "Every dependency must be registered");

/*No circular dependencies*/
static_assert(AllNoCircularDependencies<components::ComponentTypeList>::Value, "Circular dependency detected");
} // namespace object

/*
 * All vaidation for widget components
 */
namespace widget
{

}
} // namespace component::validation
