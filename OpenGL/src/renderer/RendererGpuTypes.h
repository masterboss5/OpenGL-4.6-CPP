#pragma once

#include <glm.hpp>

#include "src/types.h"

namespace renderer
{
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
		ShadowData = 8
	};

	struct alignas(16) GpuFrameConstants final
	{
		glm::mat4 projection;
		glm::mat4 view;
		glm::mat4 viewProjection;
		glm::mat4 previousViewProjection;
		glm::mat4 inverseViewProjection;
		glm::vec4 cameraPositionAndNear;
		glm::vec4 renderExtentAndFar;
		glm::uvec4 countsAndFrame;
	};

	struct alignas(16) GpuInstanceRecord final
	{
		glm::mat4 transform;
		glm::mat4 previousTransform;
		glm::vec4 worldBounds;
		uint32 materialIndex;
		uint32 objectID;
		uint32 batchIndex;
		uint32 flags;
	};

	struct alignas(16) GpuMaterialRecord final
	{
		uint64 baseColorTexture;
		uint64 normalTexture;
		uint64 metallicRoughnessTexture;
		uint64 occlusionTexture;
		uint64 emissiveTexture;
		uint64 transmissionTexture;
		glm::vec4 baseColorFactor;
		glm::vec4 emissiveAndMetallic;
		glm::vec4 roughnessTransmissionIor;
	};

	struct alignas(16) GpuLightRecord final
	{
		glm::vec4 positionAndRange;
		glm::vec4 directionAndType;
		glm::vec4 colorAndIntensity;
		glm::vec4 spotAnglesAndShadow;
	};

	struct GpuClusterHeader final { uint32 offset = 0; uint32 count = 0; uint32 pad0 = 0; uint32 pad1 = 0; };
	struct GpuShadowRecord final { glm::mat4 viewProjection; glm::vec4 atlasScaleBias; glm::vec4 depthBiasAndFilter; };
	static_assert(sizeof(GpuFrameConstants) == sizeof(glm::mat4) * 5U + sizeof(glm::vec4) * 3U);
	static_assert(sizeof(GpuMaterialRecord) == sizeof(uint64) * 6U + sizeof(glm::vec4) * 3U);
	static_assert(sizeof(GpuClusterHeader) == sizeof(uint32) * 4);
}
