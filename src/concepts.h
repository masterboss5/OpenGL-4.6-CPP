#pragma once
#include <concepts>
#include "src/resource/Asset.h"
#include "src/resource/asset/importer/AssetImporter.h"

template<typename T>
concept IsAsset = std::derived_from<T, resource::Asset>;

template<typename T>
concept IsAssetImporter = std::derived_from<T, resource::importer::AssetImporter>;