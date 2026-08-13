#pragma once

#include "Source/core/EngineAPI.h"

#include "Source/pipeline/graph/RenderGraph.h"

namespace pipeline::graph
{
struct HybridDeferredFrameInputs final
{
	Extent2D Extent;
	BufferHandle CandidateInstances;
	BufferHandle ShadowInstances;
	BufferHandle VisibleInstances;
	BufferHandle IndirectCommands;
	BufferHandle BatchMetadata;
	BufferHandle VisibilityScratch;
	BufferHandle SelectionMask;
	uint32 SelectionMaskWordCount = 0;
	uint32 DirectionalShadowResolution = 2'048;
	uint32 DirectionalShadowLayerCount = 0;
	uint32 SpotShadowLayerCount = 0;
	uint32 PointShadowFaceLayerCount = 0;
	uint32 TemporalHistoryWriteIndex = 0;
	string_view HistoryNamespace;
	bool AccuratePickingEnabled = false;
};

struct HybridDeferredFrameResources final
{
	TextureHandle DirectionalShadowAtlas;
	TextureHandle SpotShadowAtlas;
	TextureHandle PointShadowArray;
	TextureHandle Depth;
	TextureHandle HierarchicalDepthRead;
	TextureHandle HierarchicalDepthWrite;
	TextureHandle GBufferBaseColor;
	TextureHandle GBufferNormalRoughness;
	TextureHandle GBufferMaterial;
	TextureHandle GBufferOcclusion;
	TextureHandle Velocity;
	TextureHandle ObjectID;
	TextureHandle AccuratePickDepth;
	TextureHandle AccuratePickObjectID;
	TextureHandle Overdraw;
	TextureHandle HDRLighting;
	TextureHandle TransparencyAccumulation;
	TextureHandle TransparencyRevealage;
	TextureHandle CompositedHDR;
	TextureHandle TAAHistoryRead;
	TextureHandle TAAHistoryWrite;
	TextureHandle TAAObjectIDHistoryRead;
	TextureHandle TAAObjectIDHistoryWrite;
	TextureHandle TAAResolved;
	TextureHandle VisualizedHDR;
	TextureHandle OutlinedHDR;
	TextureHandle Exposure;
	TextureHandle Bloom;
	TextureHandle Presentation;
	BufferHandle ClusterHeaders;
	BufferHandle ClusterIndices;
};

enum class HybridDeferredPassID : uint8
{
	DirectionalShadows,
	SpotShadows,
	PointShadows,
	MainVisibility,
	AccuratePicking,
	DepthPrepass,
	HierarchicalDepth,
	GBuffer,
	ClusteredLights,
	DeferredLighting,
	WeightedOIT,
	OITComposition,
	TemporalAA,
	ViewportVisualization,
	SelectionOutline,
	WireframeOverlay,
	EditorGrid,
	DebugOverlay,
	GizmoOverlay,
	ExposureAndBloom,
	ToneMapAndPresent
};

using HybridDeferredPassExecutor = void (*)(void *, HybridDeferredPassID, RenderGraphContext &, const HybridDeferredFrameResources &);

struct HybridDeferredPassCallback final
{
	HybridDeferredPassExecutor Executor = nullptr;
	void *UserData = nullptr;
	HybridDeferredPassID Pass = HybridDeferredPassID::DirectionalShadows;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return this->Executor != nullptr && this->UserData != nullptr;
	}

	void operator()(RenderGraphContext &Context, const HybridDeferredFrameResources &Resources) const
	{
		if (!*this)
			throw std::logic_error("Hybrid deferred pass callback is not initialized");
		this->Executor(this->UserData, this->Pass, Context, Resources);
	}
};

// Every callback is mandatory.  This prevents an apparently complete frame
// graph from shipping with an unimplemented render stage.
struct HybridDeferredPassCallbacks final
{
	HybridDeferredPassCallback DirectionalShadows;
	HybridDeferredPassCallback SpotShadows;
	HybridDeferredPassCallback PointShadows;
	HybridDeferredPassCallback MainVisibility;
	HybridDeferredPassCallback AccuratePicking;
	HybridDeferredPassCallback DepthPrepass;
	HybridDeferredPassCallback HierarchicalDepth;
	HybridDeferredPassCallback GBuffer;
	HybridDeferredPassCallback ClusteredLights;
	HybridDeferredPassCallback DeferredLighting;
	HybridDeferredPassCallback WeightedOIT;
	HybridDeferredPassCallback OITComposition;
	HybridDeferredPassCallback TemporalAA;
	HybridDeferredPassCallback ViewportVisualization;
	HybridDeferredPassCallback SelectionOutline;
	HybridDeferredPassCallback WireframeOverlay;
	HybridDeferredPassCallback EditorGrid;
	HybridDeferredPassCallback DebugOverlay;
	HybridDeferredPassCallback GizmoOverlay;
	HybridDeferredPassCallback ExposureAndBloom;
	HybridDeferredPassCallback ToneMapAndPresent;
};

class ENGINE_API HybridDeferredFrameGraph final
{
  public:
	[[nodiscard]] HybridDeferredFrameResources Build(RenderGraph &Graph, const HybridDeferredFrameInputs &Inputs,
													 const HybridDeferredPassCallbacks &Callbacks) const;
	void BuildInto(RenderGraph &Graph, const HybridDeferredFrameInputs &Inputs, const HybridDeferredPassCallbacks &Callbacks,
				   HybridDeferredFrameResources &Resources) const;

  private:
	[[nodiscard]] static uint32 CalculateMipCount(Extent2D Extent);
	static void RequireCallback(const HybridDeferredPassCallback &Callback, string_view PassName);
	void BuildImpl(RenderGraph &Graph, const HybridDeferredFrameInputs &Inputs, const HybridDeferredPassCallbacks &Callbacks,
				   HybridDeferredFrameResources &Resources) const;
};
} // namespace pipeline::graph
