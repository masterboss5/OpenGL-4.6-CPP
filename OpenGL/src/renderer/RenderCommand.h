#pragma once

#include "src/renderer/RendererGpuTypes.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <glm.hpp>

namespace renderer
{
class VertexDescriptor;
enum class RenderIndexFormat : uint8
{
	UInt16,
	UInt32
};

enum class RenderPassClass : uint8
{
	Shadow,
	Depth,
	GBuffer,
	Transparency
};

struct RenderItem final
{
	uint32 VertexArray = 0;
	const VertexDescriptor *VertexDescriptor = nullptr;
	RenderIndexFormat IndexFormat = RenderIndexFormat::UInt32;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	int32 BaseVertex = 0;
	util::UUID MaterialGeneration;
	GPUMaterialRecord Material;
	glm::mat4 Transform{1.0f};
	glm::mat4 PreviousTransform{1.0f};
	glm::vec4 WorldBounds{0.0f, 0.0f, 0.0f, 1.0f};
	uint32 ObjectID = 0;
	uint32 LayerMask = ~uint32{0};
	uint64 Revision = 0;
	uint32 SkinPaletteOffset = 0;
	uint32 PreviousSkinPaletteOffset = 0;
	uint32 MorphDeltaBuffer = 0;
	uint32 MorphVertexCount = 0;
	uint32 MorphWeightOffset = 0;
	uint32 MorphWeightCount = 0;
	bool Skinned = false;
	bool Transparent = false;
	bool CastsShadows = true;
	bool ReceivesShadows = true;
	bool Masked = false;
	bool TwoSided = false;
};

struct PreparedInstance final
{
	glm::mat4 Transform{1.0f};
	glm::mat4 PreviousTransform{1.0f};
	glm::vec4 WorldBounds{0.0f, 0.0f, 0.0f, 1.0f};
	uint32 MaterialIndex = 0;
	uint32 ObjectID = 0;
	uint32 BatchIndex = 0;
	uint32 SkinPaletteOffset = 0;
	uint32 PreviousSkinPaletteOffset = 0;
	uint32 Flags = 0;
	uint32 MorphWeightOffset = 0;
	uint32 MorphWeightCount = 0;
};
static_assert(sizeof(PreparedInstance) == sizeof(GPUInstanceRecord),
			  "PreparedInstance must remain binary-compatible with the shader instance record");

struct RenderBatch final
{
	RenderPassClass PassClass = RenderPassClass::GBuffer;
	const VertexDescriptor *VertexDescriptor = nullptr;
	uint32 PipelineIndex = 0;
	uint32 VertexArray = 0;
	uint32 IndexCount = 0;
	uint32 FirstIndex = 0;
	int32 BaseVertex = 0;
	uint32 FirstCandidate = 0;
	uint32 CandidateCount = 0;
	RenderIndexFormat IndexFormat = RenderIndexFormat::UInt32;
	uint32 MorphDeltaBuffer = 0;
	uint32 MorphVertexCount = 0;
	bool TwoSided = false;
};

// Binary-compatible with DrawElementsIndirectCommand, but deliberately uses
// engine aliases so it can be validated and stored independently of OpenGL.
struct RenderCommand final
{
	uint32 IndexCount = 0;
	uint32 InstanceCount = 0;
	uint32 FirstIndex = 0;
	int32 BaseVertex = 0;
	uint32 BaseInstance = 0;
};

static_assert(sizeof(RenderCommand) == sizeof(uint32) * 5);
} // namespace renderer
