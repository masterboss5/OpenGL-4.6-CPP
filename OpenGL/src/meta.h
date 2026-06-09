#pragma once

#include <cstddef>
#include <type_traits>

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
	static_assert(false, "Type is not found in type list");
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







//struct 