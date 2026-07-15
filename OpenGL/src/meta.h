#pragma once

#include <cstddef>
#include <type_traits>
#include <concepts>

template<typename>
inline constexpr bool dependent_false_v = false;

template<typename... Ts>
struct TypeList
{
	static constexpr std::size_t size = sizeof...(Ts);

	template<typename... T>
	using Append = TypeList<Ts..., T...>;
};

template<typename T, typename List, std::size_t Index = 0>
struct TypeListIndex;

template<typename T, std::size_t Index>
struct TypeListIndex<T, TypeList<>, Index>
{
	static_assert(dependent_false_v<T>, "Type is not found in type list");
};

template<typename T, typename... Rest, std::size_t Index>
struct TypeListIndex<T, TypeList<T, Rest...>, Index>
{
	static constexpr std::size_t value = Index;
};

template<typename T, typename K, typename... Rest, std::size_t Index>
struct TypeListIndex<T, TypeList<K, Rest...>, Index>
{
private:
	using Next = TypeListIndex<T, TypeList<Rest...>, Index + 1>;
public:
	static constexpr std::size_t value = Next::value;
};

template<typename T, typename List>
struct TypeListContains;

template<typename T>
struct TypeListContains<T, TypeList<>>
{
	static constexpr bool value = false;
};

template<typename T, typename... Rest>
struct TypeListContains<T, TypeList<T, Rest...>>
{
	static constexpr bool value = true;
};

template<typename T, typename K, typename... Rest>
struct TypeListContains<T, TypeList<K, Rest...>>
{
private:
	using Next = TypeListContains<T, TypeList<Rest...>>;

public:
	static constexpr bool value = Next::value;
};

template<typename List>
struct TypeListUnique;

template<>
struct TypeListUnique<TypeList<>>
{
	static constexpr bool value = true;
};

template<typename T, typename... Rest>
struct TypeListUnique<TypeList<T, Rest...>>
{
	static constexpr bool value =
		!TypeListContains<T, TypeList<Rest...>>::value &&
		TypeListUnique<TypeList<Rest...>>::value;
};

template<typename T, typename List>
struct TypeListSubtypeOf;

template<typename T, typename... Rest>
struct TypeListSubtypeOf<T, TypeList<Rest...>>
{
	static constexpr bool value =
		(std::derived_from<Rest, T> && ...) &&
		(!std::same_as<Rest, T> && ...);
};

template<typename Base, typename List>
struct TypeListAllDerivedFrom;

template<typename Base>
struct TypeListAllDerivedFrom<Base, TypeList<>>
{
	static constexpr bool value = true;
};

template<typename Base, typename T, typename... Rest>
struct TypeListAllDerivedFrom<Base, TypeList<T, Rest...>>
{
	static constexpr bool value = TypeListAllDerivedFrom<Base, TypeList<Rest...>>::value &&
		std::is_base_of_v<Base, T> && 
		!std::is_same_v<Base, T>;
};

template<typename List>
struct TypeListAllNotAbstract;

template<>
struct TypeListAllNotAbstract<TypeList<>>
{
	static constexpr bool value = true;
};

template<typename T, typename... Rest>
struct TypeListAllNotAbstract<TypeList<T, Rest...>>
{
	static constexpr bool value = TypeListAllNotAbstract<TypeList<Rest...>>::value && !std::is_abstract_v<T>;
};

template<typename List>
struct TypeListAllFinal;

template<>
struct TypeListAllFinal<TypeList<>>
{
	static constexpr bool value = true;
};

template<typename T, typename... Rest>
struct TypeListAllFinal<TypeList<T, Rest...>>
{
	static constexpr bool value = TypeListAllFinal<TypeList<Rest...>>::value && std::is_final_v<T>;
};
