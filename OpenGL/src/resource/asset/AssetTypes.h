#pragma once

#include "src/types.h"

namespace resource
{
using AssetID = string;

enum class AssetType
{
	Texture2D,
	Material,
	MaterialInstance,
	Model,
	StaticMesh,
	SkeletalMesh,
	Skeleton,
	AnimationClip,
	AnimationGraph,
	RetargetProfile,
	ShaderSource,
	Count
};
} // namespace resource
