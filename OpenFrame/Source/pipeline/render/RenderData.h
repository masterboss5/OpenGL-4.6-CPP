#pragma once

#include "Source/types.h"

#include <glm.hpp>
#include <algorithm>
#include <cstddef>

namespace pipeline::render
{
inline constexpr uint32 MaximumLightCount = 100;
inline constexpr uint32 ClusterTileCountX = 32;
inline constexpr uint32 ClusterTileCountY = 18;
inline constexpr uint32 ClusterDepthSliceCount = 24;
inline constexpr uint32 ClusterCount = ClusterTileCountX * ClusterTileCountY * ClusterDepthSliceCount;
inline constexpr uint32 MaximumLightsPerCluster = MaximumLightCount;
inline constexpr uint32 DirectionalShadowCascadeCount = 4;
inline constexpr uint32 MaximumSpotShadowCount = 64;
inline constexpr uint32 MaximumPointShadowCount = 16;
inline constexpr uint32 MaximumPointShadowFaceCount = MaximumPointShadowCount * 6U;
inline constexpr uint32 MaximumShadowRecordCount = DirectionalShadowCascadeCount + MaximumSpotShadowCount + MaximumPointShadowFaceCount;
inline constexpr uint32 DirectionalShadowResolution = 2048;
inline constexpr uint32 MinimumDirectionalShadowResolution = 256;
inline constexpr uint32 MaximumDirectionalShadowResolution = 8192;
inline constexpr uint64 DirectionalShadowMemoryBudgetBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr uint32 SpotShadowResolution = 1024;
inline constexpr uint32 PointShadowResolution = 512;
inline constexpr uint32 MinimumSpotShadowResolution = 256;
inline constexpr uint32 MinimumPointShadowResolution = 256;
inline constexpr uint64 SpotShadowMemoryBudgetBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr uint64 PointShadowMemoryBudgetBytes = 32ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr uint32 CalculateShadowResolution(const uint32 MaximumResolution, const uint32 MinimumResolution,

														 const uint32 LayerCount, const uint64 MemoryBudgetBytes) noexcept
{
	const uint64 EffectiveLayerCount = std::max<uint64>(LayerCount, 1U);
	uint32 Resolution = MaximumResolution;
	while (Resolution > MinimumResolution)
	{
		const uint64 RequiredBytes =
			static_cast<uint64>(Resolution) * static_cast<uint64>(Resolution) * EffectiveLayerCount * sizeof(float32);
		if (RequiredBytes <= MemoryBudgetBytes)
			break;
		Resolution /= 2U;
	}
	return std::max(Resolution, MinimumResolution);
}

[[nodiscard]] constexpr uint32 CalculateSpotShadowResolution(const uint32 LayerCount) noexcept
{
	return CalculateShadowResolution(SpotShadowResolution, MinimumSpotShadowResolution, LayerCount, SpotShadowMemoryBudgetBytes);
}

[[nodiscard]] constexpr uint32 CalculatePointShadowResolution(const uint32 FaceLayerCount) noexcept
{
	return CalculateShadowResolution(PointShadowResolution, MinimumPointShadowResolution, FaceLayerCount, PointShadowMemoryBudgetBytes);
}

static_assert(CalculateSpotShadowResolution(MaximumSpotShadowCount) == 512U);
static_assert(CalculatePointShadowResolution(MaximumPointShadowFaceCount) == 256U);

// Fixed engine bindings. Shader reflection must reject any stage that uses
// these bindings with an incompatible block layout.
enum class RendererBinding : uint32
{
	FrameConstants = 0,
	Instances = 0,
	Materials = 1,
	Lights = 2,
	ClusterHeaders = 3,
	ClusterIndices = 4,
	Candidates = 5,
	VisibilityScratch = 6,
	IndirectCommands = 7,
	ShadowData = 8,
	SkinMatrices = 9,
	MorphDeltas = 10,
	MorphWeights = 11,
	SelectionMask = 12,
	DebugLines = 13
};

enum class GPUInstanceFlag : uint32
{
	Transparent = 1U << 0U,
	Skinned = 1U << 1U,
	Morphed = 1U << 2U,
	CastsShadows = 1U << 3U,
	ReceivesShadows = 1U << 4U,
	Masked = 1U << 5U,
	TwoSided = 1U << 6U
};

struct alignas(16) GPUFrameConstants final
{
	glm::mat4 Projection;
	glm::mat4 View;
	glm::mat4 ViewProjection;
	glm::mat4 PreviousViewProjection;
	glm::mat4 InverseViewProjection;
	glm::vec4 CameraPositionAndNear;
	glm::vec4 RenderExtentAndFar;
	glm::uvec4 CountsAndFrame;
	glm::vec4 BackgroundColor;
};

struct alignas(16) GPUInstanceRecord final
{
	glm::mat4 Transform;
	glm::mat4 PreviousTransform;
	glm::vec4 WorldBounds;
	uint32 MaterialIndex;
	uint32 ObjectID;
	uint32 BatchIndex;
	uint32 SkinPaletteOffset;
	uint32 PreviousSkinPaletteOffset;
	uint32 Flags;
	uint32 MorphWeightOffset;
	uint32 MorphWeightCount;
};

struct alignas(16) GPUSkinMatrixRecord final
{
	glm::mat4 Current;
	glm::mat4 Previous;
};

struct alignas(16) GPUMorphDeltaRecord final
{
	glm::vec4 PositionDelta;
	glm::vec4 NormalDelta;
};

struct alignas(16) GPUMorphWeightRecord final
{
	uint32 DeltaOffset = 0;
	float32 CurrentWeight = 0.0f;
	float32 PreviousWeight = 0.0f;
	uint32 Padding = 0;
};

struct alignas(16) GPUMaterialRecord final
{
	uint64 BaseColorTexture;
	uint64 NormalTexture;
	uint64 MetallicRoughnessTexture;
	uint64 OcclusionTexture;
	uint64 EmissiveTexture;
	uint64 SpecularTexture;
	uint64 TransmissionTexture;
	uint64 TextureCoordinateSelectors = 0;
	glm::vec4 BaseColorFactor;
	glm::vec4 EmissiveAndMetallic;
	glm::vec4 RoughnessTransmissionIOR;
	glm::vec4 TextureControls;
};

struct alignas(16) GPULightRecord final
{
	glm::vec4 PositionAndRange;
	glm::vec4 DirectionAndType;
	glm::vec4 ColorAndIntensity;
	glm::vec4 SpotAnglesAndShadow;
};

struct GPUClusterHeader final
{
	uint32 Offset = 0;
	uint32 Count = 0;
	uint32 Pad0 = 0;
	uint32 Pad1 = 0;
};
struct GPUShadowRecord final
{
	glm::mat4 ViewProjection;
	glm::vec4 AtlasScaleBias;
	glm::vec4 DepthBiasAndFilter;
	// Directional cascades: near split, far split, active cascade count, map resolution.
	// Local-light shadow records leave this vector zeroed.
	glm::vec4 CascadeData;
};
static_assert(sizeof(GPUFrameConstants) == sizeof(glm::mat4) * 5U + sizeof(glm::vec4) * 4U);
static_assert(sizeof(GPUMaterialRecord) == 128U);
static_assert(offsetof(GPUMaterialRecord, BaseColorFactor) == 64U);
static_assert(offsetof(GPUMaterialRecord, TextureControls) == 112U);
static_assert(sizeof(GPUClusterHeader) == sizeof(uint32) * 4);
} // namespace pipeline::render
