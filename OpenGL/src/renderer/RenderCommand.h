#pragma once

#include <glm.hpp>

#include "src/types.h"

class StaticMesh;

namespace renderer
{
	class VertexDescriptor;

	enum class RenderPassClass : uint8
	{
		Shadow,
		Depth,
		GBuffer,
		Transparency
	};

	// Immutable scene submission.  It owns no GPU resource; the scene/asset layer
	// must keep mesh and material data alive until this frame has been retired.
	struct RenderItem final
	{
		const StaticMesh* mesh = nullptr;
		glm::mat4 transform { 1.0f };
		glm::vec4 worldBounds { 0.0f, 0.0f, 0.0f, 1.0f };
		uint32 objectID = 0;
		uint32 layerMask = ~uint32 { 0 };
		uint64 revision = 0;
		bool transparent = false;
	};

	struct PreparedInstance final
	{
		glm::mat4 transform { 1.0f };
		glm::mat4 previousTransform { 1.0f };
		glm::vec4 worldBounds { 0.0f, 0.0f, 0.0f, 1.0f };
		uint32 materialIndex = 0;
		uint32 objectID = 0;
		uint32 batchIndex = 0;
		uint32 flags = 0;
	};

	struct RenderBatch final
	{
		RenderPassClass passClass = RenderPassClass::GBuffer;
		const VertexDescriptor* vertexDescriptor = nullptr;
		uint32 pipelineIndex = 0;
		uint32 vertexArray = 0;
		uint32 indexCount = 0;
		uint32 firstIndex = 0;
		int32 baseVertex = 0;
		uint32 firstCandidate = 0;
		uint32 candidateCount = 0;
	};

	// Binary-compatible with DrawElementsIndirectCommand, but deliberately uses
	// engine aliases so it can be validated and stored independently of OpenGL.
	struct RenderCommand final
	{
		uint32 indexCount = 0;
		uint32 instanceCount = 0;
		uint32 firstIndex = 0;
		int32 baseVertex = 0;
		uint32 baseInstance = 0;
	};

	static_assert(sizeof(RenderCommand) == sizeof(uint32) * 5);
}
