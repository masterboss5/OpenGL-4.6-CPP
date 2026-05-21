#pragma once
#include <concepts>
#include "src/resource/Asset.h"
#include "src/resource/asset/importer/AssetImporter.h"
#include "src/component/object/CObjectComponent.h"

namespace components
{
	class CObjectComponent;
}

template<typename T>
concept IsAsset = std::derived_from<T, resource::Asset>;

template<typename T>
concept IsAssetImporter = std::derived_from<T, resource::importer::AssetImporter>;

template<typename T>
concept isCObjectComponent = std::derived_from<T, components::CObjectComponent> 
	&& !std::same_as<T, components::CObjectComponent>;