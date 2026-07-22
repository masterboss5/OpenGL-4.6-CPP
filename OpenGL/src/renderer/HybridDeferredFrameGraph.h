#pragma once

#include "RenderGraph.h"

#include <functional>

namespace renderer
{
struct HybridDeferredFrameInputs final
{
	graph::Extent2D Extent;
	graph::BufferHandle CandidateInstances;
	graph::BufferHandle ShadowInstances;
	graph::BufferHandle VisibleInstances;
	graph::BufferHandle IndirectCommands;
	graph::BufferHandle BatchMetadata;
	graph::BufferHandle VisibilityScratch;
	uint32 TemporalHistoryWriteIndex = 0;
};

struct HybridDeferredFrameResources final
{
	graph::TextureHandle DirectionalShadowAtlas;
	graph::TextureHandle SpotShadowAtlas;
	graph::TextureHandle PointShadowArray;
	graph::TextureHandle Depth;
	graph::TextureHandle HierarchicalDepth;
	graph::TextureHandle GBufferBaseColor;
	graph::TextureHandle GBufferNormalRoughness;
	graph::TextureHandle GBufferMaterial;
	graph::TextureHandle Velocity;
	graph::TextureHandle ObjectID;
	graph::TextureHandle HDRLighting;
	graph::TextureHandle TransparencyAccumulation;
	graph::TextureHandle TransparencyRevealage;
	graph::TextureHandle CompositedHDR;
	graph::TextureHandle TAAHistoryRead;
	graph::TextureHandle TAAHistoryWrite;
	graph::TextureHandle TAAResolved;
	graph::TextureHandle Exposure;
	graph::TextureHandle Bloom;
	graph::BufferHandle ClusterHeaders;
	graph::BufferHandle ClusterIndices;
};
using HybridDeferredPassCallback = std::function<void(graph::RenderGraphContext &, const HybridDeferredFrameResources &)>;

// Every callback is mandatory.  This prevents an apparently complete frame
// graph from shipping with an unimplemented render stage.
struct HybridDeferredPassCallbacks final
{
	HybridDeferredPassCallback DirectionalShadows;
	HybridDeferredPassCallback SpotShadows;
	HybridDeferredPassCallback PointShadows;
	HybridDeferredPassCallback MainVisibility;
	HybridDeferredPassCallback DepthPrepass;
	HybridDeferredPassCallback HierarchicalDepth;
	HybridDeferredPassCallback GBuffer;
	HybridDeferredPassCallback ClusteredLights;
	HybridDeferredPassCallback DeferredLighting;
	HybridDeferredPassCallback WeightedOIT;
	HybridDeferredPassCallback OITComposition;
	HybridDeferredPassCallback TemporalAA;
	HybridDeferredPassCallback ExposureAndBloom;
	HybridDeferredPassCallback ToneMapAndPresent;
};

class HybridDeferredFrameGraph final
{
  public:
	[[nodiscard]] HybridDeferredFrameResources Build(graph::RenderGraph &Graph, const HybridDeferredFrameInputs &Inputs,
													 const HybridDeferredPassCallbacks &Callbacks) const;

  private:
	[[nodiscard]] static uint32 CalculateMipCount(graph::Extent2D Extent);
	static void RequireCallback(const HybridDeferredPassCallback &Callback, string_view PassName);
};
} // namespace renderer
