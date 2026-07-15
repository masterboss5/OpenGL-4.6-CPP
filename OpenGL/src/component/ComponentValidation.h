#pragma once
#include "src/meta.h"
#include "src/component/Components.h"


 /*Must include all components*/

#include "src/component/object/CObjectTransformComponent.h"
//#include "src/component/object/CObjectMeshComponent.h"

namespace component::validation
{
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

    template<typename Components, typename Registry>
    struct AllDependenciesRegistered;

    template<typename Registry>
    struct AllDependenciesRegistered<TypeList<>, Registry>
    {
        static constexpr bool value = true;
    };

    template<typename First, typename... Rest, typename Registry>
    struct AllDependenciesRegistered<TypeList<First, Rest...>, Registry>
    {
        static constexpr bool value = DependenciesRegistered<components::GetDependencies<First>, Registry>::value &&
			AllDependenciesRegistered<TypeList<Rest...>, Registry>::value;
    };

    template<typename Dependencies, typename Visited>
    struct IterateDependencies;

    template<typename Component, typename Visited, bool AlreadyVisited = TypeListContains<Component, Visited>::value>
    struct NoCyclicDependencies;

    template<typename Component, typename Visited>
    struct NoCyclicDependencies<Component, Visited, true>
    {
        static constexpr bool value = false;
    };

    template<typename Component, typename Visited>
    struct NoCyclicDependencies<Component, Visited, false>
    {
    private:
        using Next = typename Visited::template Append<Component>;

    public:
        static constexpr bool value = IterateDependencies<components::GetDependencies<Component>, Next>::value;
    };

    template<typename Visited>
    struct IterateDependencies<TypeList<>, Visited>
    {
        static constexpr bool value = true;
    };

    template<typename Dependency, typename... Rest, typename Visited>
    struct IterateDependencies<TypeList<Dependency, Rest...>, Visited>
    {
        static constexpr bool value =
            NoCyclicDependencies<Dependency, Visited>::value &&
            IterateDependencies<TypeList<Rest...>, Visited>::value;
    };

    template<typename Dependencies>
    struct AllNoCircularDependencies;

    template<>
    struct AllNoCircularDependencies<TypeList<>>
    {
        static constexpr bool value = true;
    };

	template<typename First, typename... Rest>
	struct AllNoCircularDependencies<TypeList<First, Rest...>>
	{
    private:
        using Next = TypeList<Rest...>;

    public:
        static constexpr bool value =
            NoCyclicDependencies<First, TypeList<>>::value &&
            AllNoCircularDependencies<Next>::value;
	};

    namespace object
    {
        /*No duplicate types*/
        static_assert(
            TypeListUnique<
            components::ComponentTypeList
            >::value,
            "Duplicate component type"
            );

        /*All components must be final*/
        static_assert(
            TypeListAllFinal<
            components::ComponentTypeList
            >::value,
            "Every registered component must be final"
            );

        /*No abstract components*/
        static_assert(
            TypeListAllNotAbstract<
            components::ComponentTypeList
            >::value,
            "Registered components cannot be abstract"
            );

        /*All must be a CObjectComponent*/
        static_assert(
            TypeListAllDerivedFrom<
            components::CObjectComponent,
            components::ComponentTypeList
            >::value,
            "Every registered component must derive from CObjectComponent"
            );

        /*Every dependency must be registered*/
        static_assert(
            AllDependenciesRegistered<
            components::ComponentTypeList,
            components::ComponentTypeList
            >::value,
            "Every dependency must be registered"
            );

        /*No circular dependencies*/
        static_assert(
            AllNoCircularDependencies<
            components::ComponentTypeList
            >::value,
            "Circular dependency detected"
            );
    }

    /*
    * All vaidation for widget components
    */
    namespace widget
    {

    }
}