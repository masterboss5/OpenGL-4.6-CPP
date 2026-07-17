#pragma once

#include <functional>

#include "RenderGraph.h"

namespace renderer
{
	struct HybridDeferredFrameInputs final
	{
		graph::Extent2D extent;
		graph::BufferHandle candidateInstances;
		graph::BufferHandle visibleInstances;
		graph::BufferHandle indirectCommands;
		graph::BufferHandle batchMetadata;
		graph::BufferHandle visibilityScratch;
	};

	struct HybridDeferredFrameResources final
	{
		graph::TextureHandle directionalShadowAtlas;
		graph::TextureHandle spotShadowAtlas;
		graph::TextureHandle pointShadowArray;
		graph::TextureHandle depth;
		graph::TextureHandle hierarchicalDepth;
		graph::TextureHandle gbufferBaseColor;
		graph::TextureHandle gbufferNormalRoughness;
		graph::TextureHandle gbufferMaterial;
		graph::TextureHandle velocity;
		graph::TextureHandle objectID;
		graph::TextureHandle hdrLighting;
		graph::TextureHandle transparencyAccumulation;
		graph::TextureHandle transparencyRevealage;
		graph::TextureHandle compositedHDR;
		graph::TextureHandle taaHistory;
		graph::TextureHandle taaResolved;
		graph::TextureHandle exposure;
		graph::TextureHandle bloom;
		graph::BufferHandle clusterHeaders;
		graph::BufferHandle clusterIndices;
	};
	using HybridDeferredPassCallback = std::function<void(graph::RenderGraphContext&, const HybridDeferredFrameResources&)>;

	// Every callback is mandatory.  This prevents an apparently complete frame
	// graph from shipping with an unimplemented render stage.
	struct HybridDeferredPassCallbacks final
	{
		HybridDeferredPassCallback directionalShadows;
		HybridDeferredPassCallback spotShadows;
		HybridDeferredPassCallback pointShadows;
		HybridDeferredPassCallback mainVisibility;
		HybridDeferredPassCallback depthPrepass;
		HybridDeferredPassCallback hierarchicalDepth;
		HybridDeferredPassCallback gbuffer;
		HybridDeferredPassCallback clusteredLights;
		HybridDeferredPassCallback deferredLighting;
		HybridDeferredPassCallback weightedOIT;
		HybridDeferredPassCallback oitComposition;
		HybridDeferredPassCallback temporalAA;
		HybridDeferredPassCallback exposureAndBloom;
		HybridDeferredPassCallback toneMapAndPresent;
	};

	class HybridDeferredFrameGraph final
	{
	public:
		[[nodiscard]] HybridDeferredFrameResources build(graph::RenderGraph& graph, const HybridDeferredFrameInputs& inputs, const HybridDeferredPassCallbacks& callbacks) const;
	private:
		[[nodiscard]] static uint32 calculateMipCount(graph::Extent2D extent);
		static void requireCallback(const HybridDeferredPassCallback& callback, string_view passName);
	};
}
