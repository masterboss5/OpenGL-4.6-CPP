#pragma once

#include "src/types.h"

#include <glm.hpp>
#include <cstddef>

namespace renderer
{
inline constexpr uint32 DirectionalShadowCascadeCount = 4;
inline constexpr uint32 MaximumSpotShadowCount = 64;
inline constexpr uint32 MaximumPointShadowCount = 16;
inline constexpr uint32 MaximumPointShadowFaceCount = MaximumPointShadowCount * 6U;
inline constexpr uint32 MaximumShadowRecordCount = DirectionalShadowCascadeCount + MaximumSpotShadowCount + MaximumPointShadowFaceCount;

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
	MorphWeights = 11
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
};
static_assert(sizeof(GPUFrameConstants) == sizeof(glm::mat4) * 5U + sizeof(glm::vec4) * 4U);
static_assert(sizeof(GPUMaterialRecord) == 128U);
static_assert(offsetof(GPUMaterialRecord, BaseColorFactor) == 64U);
static_assert(offsetof(GPUMaterialRecord, TextureControls) == 112U);
static_assert(sizeof(GPUClusterHeader) == sizeof(uint32) * 4);
} // namespace renderer
