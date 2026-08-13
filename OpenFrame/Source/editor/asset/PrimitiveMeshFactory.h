#pragma once

#include "Source/resource/asset/AssetHandle.h"
#include "Source/resource/asset/ModelAsset.h"
#include "Source/types.h"

#include <array>

namespace resource
{
class AssetManager;
}

namespace editor::asset
{
enum class PrimitiveShape : uint8
{
	Box,
	Sphere,
	Capsule,
	Cylinder,
	Cone,
	Plane,
	Count
};

class PrimitiveMeshFactory final
{
  public:
	explicit PrimitiveMeshFactory(resource::AssetManager &Assets);

	[[nodiscard]] resource::AssetHandle<resource::ModelAsset> GetModel(PrimitiveShape Shape);
	[[nodiscard]] static string_view GetName(PrimitiveShape Shape);
	[[nodiscard]] static resource::AssetID GetModelID(PrimitiveShape Shape);

  private:
	[[nodiscard]] resource::AssetHandle<resource::ModelAsset> Build(PrimitiveShape Shape);

	resource::AssetManager *Assets = nullptr;
	std::array<resource::AssetHandle<resource::ModelAsset>, static_cast<usize>(PrimitiveShape::Count)> Models;
	resource::AssetHandle<resource::MaterialAsset> DefaultMaterial;
};
} // namespace editor::asset
