#pragma once

#include "src/types.h"

#include <concepts>
#include <cstddef>
#include <type_traits>

template <typename> inline constexpr bool DependentFalseV = false;

template <typename... Ts> struct TypeList
{
	static constexpr usize Size = sizeof...(Ts);

	template <typename... T> using Append = TypeList<Ts..., T...>;
};

template <typename T, typename List, usize Index = 0> struct TypeListIndex;

template <typename T, usize Index> struct TypeListIndex<T, TypeList<>, Index>
{
	static_assert(DependentFalseV<T>, "Type is not found in type list");
};

template <typename T, typename... Rest, usize Index> struct TypeListIndex<T, TypeList<T, Rest...>, Index>
{
	static constexpr usize Value = Index;
};

template <typename T, typename K, typename... Rest, usize Index> struct TypeListIndex<T, TypeList<K, Rest...>, Index>
{
  private:
	using Next = TypeListIndex<T, TypeList<Rest...>, Index + 1>;

  public:
	static constexpr usize Value = Next::Value;
};

template <typename T, typename List> struct TypeListContains;

template <typename T> struct TypeListContains<T, TypeList<>>
{
	static constexpr bool Value = false;
};

template <typename T, typename... Rest> struct TypeListContains<T, TypeList<T, Rest...>>
{
	static constexpr bool Value = true;
};

template <typename T, typename K, typename... Rest> struct TypeListContains<T, TypeList<K, Rest...>>
{
  private:
	using Next = TypeListContains<T, TypeList<Rest...>>;

  public:
	static constexpr bool Value = Next::Value;
};

template <typename List> struct TypeListUnique;

template <> struct TypeListUnique<TypeList<>>
{
	static constexpr bool Value = true;
};

template <typename T, typename... Rest> struct TypeListUnique<TypeList<T, Rest...>>
{
	static constexpr bool Value = !TypeListContains<T, TypeList<Rest...>>::Value && TypeListUnique<TypeList<Rest...>>::Value;
};

template <typename T, typename List> struct TypeListSubtypeOf;

template <typename T, typename... Rest> struct TypeListSubtypeOf<T, TypeList<Rest...>>
{
	static constexpr bool Value = (std::derived_from<Rest, T> && ...) && (!std::same_as<Rest, T> && ...);
};

template <typename Base, typename List> struct TypeListAllDerivedFrom;

template <typename Base> struct TypeListAllDerivedFrom<Base, TypeList<>>
{
	static constexpr bool Value = true;
};

template <typename Base, typename T, typename... Rest> struct TypeListAllDerivedFrom<Base, TypeList<T, Rest...>>
{
	static constexpr bool Value =
		TypeListAllDerivedFrom<Base, TypeList<Rest...>>::Value && std::is_base_of_v<Base, T> && !std::is_same_v<Base, T>;
};

template <typename List> struct TypeListAllNotAbstract;

template <> struct TypeListAllNotAbstract<TypeList<>>
{
	static constexpr bool Value = true;
};

template <typename T, typename... Rest> struct TypeListAllNotAbstract<TypeList<T, Rest...>>
{
	static constexpr bool Value = TypeListAllNotAbstract<TypeList<Rest...>>::Value && !std::is_abstract_v<T>;
};

template <typename List> struct TypeListAllFinal;

template <> struct TypeListAllFinal<TypeList<>>
{
	static constexpr bool Value = true;
};

template <typename T, typename... Rest> struct TypeListAllFinal<TypeList<T, Rest...>>
{
	static constexpr bool Value = TypeListAllFinal<TypeList<Rest...>>::Value && std::is_final_v<T>;
};
