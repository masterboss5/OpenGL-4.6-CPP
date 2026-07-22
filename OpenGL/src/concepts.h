#pragma once
#include <concepts>
#include <type_traits>

namespace components
{
class CObjectComponent;
template <typename T> struct ComponentRegistration;
} // namespace components

class ApplicationLayer;
struct DirectionalLightSource;
struct PointLightSource;
struct SpotLightSource;

namespace resource
{
class Asset;
namespace importer
{
class AssetImporter;
}
} // namespace resource

template <typename T>
concept IsAsset = std::derived_from<std::remove_cvref_t<T>, resource::Asset>;

template <typename T>
concept IsAssetImporter = std::derived_from<std::remove_cvref_t<T>, resource::importer::AssetImporter>;

template <typename T>
concept IsAssetWithStaticType = IsAsset<T> && requires { std::remove_cvref_t<T>::AssetType; };

template <typename T>
concept IsCObjectComponent = components::ComponentRegistration<std::remove_cvref_t<T>>::value;

template <typename T>
concept IsApplicationLayer = std::derived_from<std::remove_cvref_t<T>, ApplicationLayer>;

template <typename T>
concept TriviallyCopyable = std::is_trivially_copyable_v<std::remove_cvref_t<T>>;

template <typename T>
concept PoolAllocatable = std::is_object_v<T> && std::is_nothrow_destructible_v<T> && !std::is_array_v<T> && !std::is_abstract_v<T> &&
						  !std::is_const_v<T> && !std::is_volatile_v<T>;

template <typename T>
concept IsLightSource = std::same_as<std::remove_cvref_t<T>, DirectionalLightSource> ||
						std::same_as<std::remove_cvref_t<T>, PointLightSource> || std::same_as<std::remove_cvref_t<T>, SpotLightSource>;

template <typename T>
concept IsAttenuatedLightSource =
	std::same_as<std::remove_cvref_t<T>, PointLightSource> || std::same_as<std::remove_cvref_t<T>, SpotLightSource>;

template <typename T>
concept COMInterface = requires(T *Interface) {
	Interface->AddRef();
	Interface->Release();
};

template <typename T>
concept FunctionPointer = std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

template <typename T>
concept Integral = std::integral<std::remove_cvref_t<T>>;

template <typename T>
concept FloatingPoint = std::floating_point<std::remove_cvref_t<T>>;

template <typename T>
concept Arithmetic = Integral<T> || FloatingPoint<T>;

template <typename T>
concept Enumeration = std::is_enum_v<std::remove_cvref_t<T>>;
