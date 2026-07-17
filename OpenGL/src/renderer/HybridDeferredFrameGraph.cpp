#include "HybridDeferredFrameGraph.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace renderer
{
	uint32 HybridDeferredFrameGraph::calculateMipCount(graph::Extent2D extent)
	{
		uint32 dimension = std::max(extent.width, extent.height);
		uint32 mipCount = 1;
		while (dimension > 1) { dimension >>= 1U; ++mipCount; }
		return mipCount;
	}

	void HybridDeferredFrameGraph::requireCallback(const HybridDeferredPassCallback& callback, string_view passName)
	{
		if (!callback) throw std::invalid_argument("Hybrid deferred frame graph requires callback for " + std::string(passName));
	}

	HybridDeferredFrameResources HybridDeferredFrameGraph::build(graph::RenderGraph& graph, const HybridDeferredFrameInputs& inputs, const HybridDeferredPassCallbacks& callbacks) const
	{
		if (!inputs.extent.isValid()) throw std::invalid_argument("Hybrid deferred frame graph requires a valid extent");
		requireCallback(callbacks.directionalShadows, "DirectionalShadows"); requireCallback(callbacks.spotShadows, "SpotShadows"); requireCallback(callbacks.pointShadows, "PointShadows"); requireCallback(callbacks.mainVisibility, "MainVisibility"); requireCallback(callbacks.depthPrepass, "DepthPrepass"); requireCallback(callbacks.hierarchicalDepth, "HierarchicalDepth"); requireCallback(callbacks.gbuffer, "GBuffer"); requireCallback(callbacks.clusteredLights, "ClusteredLights"); requireCallback(callbacks.deferredLighting, "DeferredLighting"); requireCallback(callbacks.weightedOIT, "WeightedOIT"); requireCallback(callbacks.oitComposition, "OITComposition"); requireCallback(callbacks.temporalAA, "TemporalAA"); requireCallback(callbacks.exposureAndBloom, "ExposureAndBloom"); requireCallback(callbacks.toneMapAndPresent, "ToneMapAndPresent");

		const graph::Extent2D shadowExtent { 4096, 4096 };
		const uint32 hierarchicalMipCount = calculateMipCount(inputs.extent);
		HybridDeferredFrameResources resources {
			.directionalShadowAtlas = graph.createTexture({ .debugName = "DirectionalShadowAtlas", .extent = shadowExtent, .format = graph::TextureFormat::Depth32Float, .dimension = graph::TextureDimension::Texture2DArray, .layers = 4, .persistent = true }),
			.spotShadowAtlas = graph.createTexture({ .debugName = "SpotShadowAtlas", .extent = shadowExtent, .format = graph::TextureFormat::Depth32Float, .dimension = graph::TextureDimension::Texture2DArray, .layers = 64, .persistent = true }),
			.pointShadowArray = graph.createTexture({ .debugName = "PointShadowArray", .extent = { 1024, 1024 }, .format = graph::TextureFormat::Depth32Float, .dimension = graph::TextureDimension::TextureCubeArray, .layers = 96, .persistent = true }),
			.depth = graph.createTexture({ .debugName = "MainDepth", .extent = inputs.extent, .format = graph::TextureFormat::Depth32Float }),
			.hierarchicalDepth = graph.createTexture({ .debugName = "HierarchicalDepth", .extent = inputs.extent, .format = graph::TextureFormat::R32Float, .mipCount = hierarchicalMipCount, .persistent = true }),
			.gbufferBaseColor = graph.createTexture({ .debugName = "GBufferBaseColor", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float }),
			.gbufferNormalRoughness = graph.createTexture({ .debugName = "GBufferNormalRoughness", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float }),
			.gbufferMaterial = graph.createTexture({ .debugName = "GBufferMaterial", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float }),
			.velocity = graph.createTexture({ .debugName = "MotionVectors", .extent = inputs.extent, .format = graph::TextureFormat::RG16Float }),
			.objectID = graph.createTexture({ .debugName = "ObjectID", .extent = inputs.extent, .format = graph::TextureFormat::R32UnsignedInteger }),
			.hdrLighting = graph.createTexture({ .debugName = "HDRLighting", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float }),
			.transparencyAccumulation = graph.createTexture({ .debugName = "OITAccumulation", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float }),
			.transparencyRevealage = graph.createTexture({ .debugName = "OITRevealage", .extent = inputs.extent, .format = graph::TextureFormat::R32Float }),
			.compositedHDR = graph.createTexture({ .debugName = "CompositedHDR", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float }),
			.taaHistory = graph.createTexture({ .debugName = "TAAHistory", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float, .persistent = true }),
			.taaResolved = graph.createTexture({ .debugName = "TAAResolved", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float }),
			.exposure = graph.createTexture({ .debugName = "AutoExposure", .extent = { 1, 1 }, .format = graph::TextureFormat::R32Float, .persistent = true }),
			.bloom = graph.createTexture({ .debugName = "Bloom", .extent = inputs.extent, .format = graph::TextureFormat::RGBA16Float, .mipCount = hierarchicalMipCount }),
			.clusterHeaders = graph.createBuffer({ .debugName = "ClusterHeaders", .sizeInBytes = static_cast<uint64>(16) * 32U * 18U * 24U }),
			.clusterIndices = graph.createBuffer({ .debugName = "ClusterIndices", .sizeInBytes = static_cast<uint64>(4) * 1'048'576U })
		};

		const auto resourceState = std::make_shared<HybridDeferredFrameResources>(resources);
		auto wrap = [resourceState](const HybridDeferredPassCallback& callback) { return [callback, resourceState](graph::RenderGraphContext& context) { callback(context, *resourceState); }; };
		auto add = [&graph, &wrap](string name, graph::PassQueue queue, std::vector<graph::TextureHandle> readTextures, std::vector<graph::BufferHandle> readBuffers, std::vector<graph::TextureHandle> writeTextures, std::vector<graph::BufferHandle> writeBuffers, const HybridDeferredPassCallback& execute) { (void)graph.addPass({ .name = std::move(name), .queue = queue, .readTextures = std::move(readTextures), .readBuffers = std::move(readBuffers), .writeTextures = std::move(writeTextures), .writeBuffers = std::move(writeBuffers), .execute = wrap(execute) }); };
		add("DirectionalShadows", graph::PassQueue::Graphics, {}, { inputs.indirectCommands }, { resources.directionalShadowAtlas }, {}, callbacks.directionalShadows);
		add("SpotShadows", graph::PassQueue::Graphics, {}, { inputs.indirectCommands }, { resources.spotShadowAtlas }, {}, callbacks.spotShadows);
		add("PointShadows", graph::PassQueue::Graphics, {}, { inputs.indirectCommands }, { resources.pointShadowArray }, {}, callbacks.pointShadows);
		add("MainVisibility", graph::PassQueue::Compute, { resources.hierarchicalDepth }, { inputs.candidateInstances, inputs.batchMetadata }, {}, { inputs.visibleInstances, inputs.indirectCommands, inputs.visibilityScratch }, callbacks.mainVisibility);
		(void)graph.addPass({ .name = "DepthPrepass", .queue = graph::PassQueue::Graphics, .readBuffers = { inputs.visibleInstances, inputs.indirectCommands }, .depthAttachment = graph::DepthAttachment { .texture = resources.depth, .load = graph::LoadOperation::Clear, .store = graph::StoreOperation::Store, .clearDepth = 0.0f }, .execute = wrap(callbacks.depthPrepass) });
		add("HierarchicalDepth", graph::PassQueue::Compute, { resources.depth }, {}, { resources.hierarchicalDepth }, {}, callbacks.hierarchicalDepth);
		(void)graph.addPass({ .name = "GBuffer", .queue = graph::PassQueue::Graphics, .readBuffers = { inputs.visibleInstances, inputs.indirectCommands }, .colorAttachments = { { .texture = resources.gbufferBaseColor, .load = graph::LoadOperation::Clear }, { .texture = resources.gbufferNormalRoughness, .load = graph::LoadOperation::Clear }, { .texture = resources.gbufferMaterial, .load = graph::LoadOperation::Clear }, { .texture = resources.velocity, .load = graph::LoadOperation::Clear }, { .texture = resources.objectID, .load = graph::LoadOperation::Clear } }, .depthAttachment = graph::DepthAttachment { .texture = resources.depth, .load = graph::LoadOperation::Load }, .execute = wrap(callbacks.gbuffer) });
		add("ClusteredLights", graph::PassQueue::Compute, { resources.depth }, {}, {}, { resources.clusterHeaders, resources.clusterIndices }, callbacks.clusteredLights);
		add("DeferredLighting", graph::PassQueue::Compute, { resources.gbufferBaseColor, resources.gbufferNormalRoughness, resources.gbufferMaterial, resources.depth, resources.directionalShadowAtlas, resources.spotShadowAtlas, resources.pointShadowArray }, { resources.clusterHeaders, resources.clusterIndices }, { resources.hdrLighting }, {}, callbacks.deferredLighting);
		(void)graph.addPass({ .name = "WeightedOIT", .queue = graph::PassQueue::Graphics, .readTextures = { resources.depth }, .readBuffers = { inputs.visibleInstances, inputs.indirectCommands, resources.clusterHeaders, resources.clusterIndices }, .colorAttachments = { { .texture = resources.transparencyAccumulation, .load = graph::LoadOperation::Clear }, { .texture = resources.transparencyRevealage, .load = graph::LoadOperation::Clear, .clearColor = glm::vec4(1.0f) } }, .depthAttachment = graph::DepthAttachment { .texture = resources.depth, .load = graph::LoadOperation::Load }, .execute = wrap(callbacks.weightedOIT) });
		add("OITComposition", graph::PassQueue::Compute, { resources.hdrLighting, resources.transparencyAccumulation, resources.transparencyRevealage }, {}, { resources.compositedHDR }, {}, callbacks.oitComposition);
		add("TemporalAA", graph::PassQueue::Compute, { resources.compositedHDR, resources.velocity, resources.depth, resources.taaHistory }, {}, { resources.taaHistory, resources.taaResolved }, {}, callbacks.temporalAA);
		add("ExposureAndBloom", graph::PassQueue::Compute, { resources.taaResolved, resources.exposure }, {}, { resources.exposure, resources.bloom }, {}, callbacks.exposureAndBloom);
		add("ToneMapAndPresent", graph::PassQueue::Graphics, { resources.taaResolved, resources.exposure, resources.bloom }, {}, {}, {}, callbacks.toneMapAndPresent);
		return *resourceState;
	}
}
