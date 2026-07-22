#include "HybridDeferredFrameGraph.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace renderer
{
uint32 HybridDeferredFrameGraph::CalculateMipCount(graph::Extent2D Extent)
{
	uint32 Dimension = std::max(Extent.Width, Extent.Height);
	uint32 MipCount = 1;
	while (Dimension > 1)
	{
		Dimension >>= 1U;
		++MipCount;
	}
	return MipCount;
}

void HybridDeferredFrameGraph::RequireCallback(const HybridDeferredPassCallback &Callback, string_view PassName)
{
	if (!Callback)
		throw std::invalid_argument("Hybrid deferred frame graph requires callback for " + std::string(PassName));
}

HybridDeferredFrameResources HybridDeferredFrameGraph::Build(graph::RenderGraph &Graph, const HybridDeferredFrameInputs &Inputs,
															 const HybridDeferredPassCallbacks &Callbacks) const
{
	if (!Inputs.Extent.IsValid())
		throw std::invalid_argument("Hybrid deferred frame graph requires a valid extent");
	RequireCallback(Callbacks.DirectionalShadows, "DirectionalShadows");
	RequireCallback(Callbacks.SpotShadows, "SpotShadows");
	RequireCallback(Callbacks.PointShadows, "PointShadows");
	RequireCallback(Callbacks.MainVisibility, "MainVisibility");
	RequireCallback(Callbacks.DepthPrepass, "DepthPrepass");
	RequireCallback(Callbacks.HierarchicalDepth, "HierarchicalDepth");
	RequireCallback(Callbacks.GBuffer, "GBuffer");
	RequireCallback(Callbacks.ClusteredLights, "ClusteredLights");
	RequireCallback(Callbacks.DeferredLighting, "DeferredLighting");
	RequireCallback(Callbacks.WeightedOIT, "WeightedOIT");
	RequireCallback(Callbacks.OITComposition, "OITComposition");
	RequireCallback(Callbacks.TemporalAA, "TemporalAA");
	RequireCallback(Callbacks.ExposureAndBloom, "ExposureAndBloom");
	RequireCallback(Callbacks.ToneMapAndPresent, "ToneMapAndPresent");

	const graph::Extent2D ShadowExtent{4096, 4096};
	const uint32 HierarchicalMipCount = CalculateMipCount(Inputs.Extent);
	const graph::TextureHandle TAAHistoryA = Graph.CreateTexture(
		{.DebugName = "TAAHistoryA", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float, .Persistent = true});
	const graph::TextureHandle TAAHistoryB = Graph.CreateTexture(
		{.DebugName = "TAAHistoryB", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float, .Persistent = true});
	const bool WriteHistoryA = (Inputs.TemporalHistoryWriteIndex & 1U) == 0U;
	HybridDeferredFrameResources Resources{
		.DirectionalShadowAtlas = Graph.CreateTexture({.DebugName = "DirectionalShadowAtlas",
													   .Extent = ShadowExtent,
													   .Format = graph::TextureFormat::Depth32Float,
													   .Dimension = graph::TextureDimension::Texture2DArray,
													   .Layers = 4,
													   .Persistent = true}),
		.SpotShadowAtlas = Graph.CreateTexture({.DebugName = "SpotShadowAtlas",
												.Extent = ShadowExtent,
												.Format = graph::TextureFormat::Depth32Float,
												.Dimension = graph::TextureDimension::Texture2DArray,
												.Layers = 64,
												.Persistent = true}),
		.PointShadowArray = Graph.CreateTexture({.DebugName = "PointShadowArray",
												 .Extent = {1024, 1024},
												 .Format = graph::TextureFormat::Depth32Float,
												 .Dimension = graph::TextureDimension::TextureCubeArray,
												 .Layers = 96,
												 .Persistent = true}),
		.Depth = Graph.CreateTexture({.DebugName = "MainDepth", .Extent = Inputs.Extent, .Format = graph::TextureFormat::Depth32Float}),
		.HierarchicalDepth = Graph.CreateTexture({.DebugName = "HierarchicalDepth",
												  .Extent = Inputs.Extent,
												  .Format = graph::TextureFormat::R32Float,
												  .MipCount = HierarchicalMipCount,
												  .Persistent = true}),
		.GBufferBaseColor =
			Graph.CreateTexture({.DebugName = "GBufferBaseColor", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float}),
		.GBufferNormalRoughness = Graph.CreateTexture(
			{.DebugName = "GBufferNormalRoughness", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float}),
		.GBufferMaterial =
			Graph.CreateTexture({.DebugName = "GBufferMaterial", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float}),
		.Velocity = Graph.CreateTexture({.DebugName = "MotionVectors", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RG16Float}),
		.ObjectID =
			Graph.CreateTexture({.DebugName = "ObjectID", .Extent = Inputs.Extent, .Format = graph::TextureFormat::R32UnsignedInteger}),
		.HDRLighting =
			Graph.CreateTexture({.DebugName = "HDRLighting", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float}),
		.TransparencyAccumulation =
			Graph.CreateTexture({.DebugName = "OITAccumulation", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float}),
		.TransparencyRevealage =
			Graph.CreateTexture({.DebugName = "OITRevealage", .Extent = Inputs.Extent, .Format = graph::TextureFormat::R32Float}),
		.CompositedHDR =
			Graph.CreateTexture({.DebugName = "CompositedHDR", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float}),
		.TAAHistoryRead = WriteHistoryA ? TAAHistoryB : TAAHistoryA,
		.TAAHistoryWrite = WriteHistoryA ? TAAHistoryA : TAAHistoryB,
		.TAAResolved =
			Graph.CreateTexture({.DebugName = "TAAResolved", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float}),
		.Exposure = Graph.CreateTexture(
			{.DebugName = "AutoExposure", .Extent = {1, 1}, .Format = graph::TextureFormat::R32Float, .Persistent = true}),
		.Bloom = Graph.CreateTexture(
			{.DebugName = "Bloom", .Extent = Inputs.Extent, .Format = graph::TextureFormat::RGBA16Float, .MipCount = HierarchicalMipCount}),
		.ClusterHeaders = Graph.CreateBuffer({.DebugName = "ClusterHeaders", .SizeInBytes = static_cast<uint64>(16) * 32U * 18U * 24U}),
		.ClusterIndices = Graph.CreateBuffer({.DebugName = "ClusterIndices", .SizeInBytes = static_cast<uint64>(4) * 1'048'576U})};

	const auto ResourceState = std::make_shared<HybridDeferredFrameResources>(Resources);
	auto Wrap = [ResourceState](const HybridDeferredPassCallback &Callback)
	{ return [Callback, ResourceState](graph::RenderGraphContext &Context) { Callback(Context, *ResourceState); }; };
	auto Add = [&Graph, &Wrap](string Name, graph::PassQueue Queue, std::vector<graph::TextureHandle> ReadTextures,
							   std::vector<graph::BufferHandle> ReadBuffers, std::vector<graph::TextureHandle> WriteTextures,
							   std::vector<graph::BufferHandle> WriteBuffers, const HybridDeferredPassCallback &Execute)
	{
		(void)Graph.AddPass({.Name = std::move(Name),
							 .Queue = Queue,
							 .ReadTextures = std::move(ReadTextures),
							 .ReadBuffers = std::move(ReadBuffers),
							 .WriteTextures = std::move(WriteTextures),
							 .WriteBuffers = std::move(WriteBuffers),
							 .Execute = Wrap(Execute)});
	};
	(void)Graph.AddPass({.Name = "DirectionalShadows",
						 .Queue = graph::PassQueue::Graphics,
						 .ReadBuffers = {Inputs.ShadowInstances},
						 .DepthAttachment = graph::DepthAttachment{.Texture = Resources.DirectionalShadowAtlas,
																   .Load = graph::LoadOperation::Load,
																   .Store = graph::StoreOperation::Store,
																   .ClearDepth = 0.0f},
						 .Execute = Wrap(Callbacks.DirectionalShadows)});
	(void)Graph.AddPass({.Name = "SpotShadows",
						 .Queue = graph::PassQueue::Graphics,
						 .ReadBuffers = {Inputs.ShadowInstances},
						 .DepthAttachment = graph::DepthAttachment{.Texture = Resources.SpotShadowAtlas,
																   .Load = graph::LoadOperation::Load,
																   .Store = graph::StoreOperation::Store,
																   .ClearDepth = 0.0f},
						 .Execute = Wrap(Callbacks.SpotShadows)});
	(void)Graph.AddPass({.Name = "PointShadows",
						 .Queue = graph::PassQueue::Graphics,
						 .ReadBuffers = {Inputs.ShadowInstances},
						 .DepthAttachment = graph::DepthAttachment{.Texture = Resources.PointShadowArray,
																   .Load = graph::LoadOperation::Load,
																   .Store = graph::StoreOperation::Store,
																   .ClearDepth = 0.0f},
						 .Execute = Wrap(Callbacks.PointShadows)});
	Add("MainVisibility", graph::PassQueue::Compute, {Resources.HierarchicalDepth}, {Inputs.CandidateInstances, Inputs.BatchMetadata}, {},
		{Inputs.VisibleInstances, Inputs.IndirectCommands, Inputs.VisibilityScratch}, Callbacks.MainVisibility);
	(void)Graph.AddPass({.Name = "DepthPrepass",
						 .Queue = graph::PassQueue::Graphics,
						 .ReadBuffers = {Inputs.VisibleInstances, Inputs.IndirectCommands},
						 .DepthAttachment = graph::DepthAttachment{.Texture = Resources.Depth,
																   .Load = graph::LoadOperation::Clear,
																   .Store = graph::StoreOperation::Store,
																   .ClearDepth = 0.0f},
						 .Execute = Wrap(Callbacks.DepthPrepass)});
	Add("HierarchicalDepth", graph::PassQueue::Compute, {Resources.Depth}, {}, {Resources.HierarchicalDepth}, {},
		Callbacks.HierarchicalDepth);
	(void)Graph.AddPass({.Name = "GBuffer",
						 .Queue = graph::PassQueue::Graphics,
						 .ReadBuffers = {Inputs.VisibleInstances, Inputs.IndirectCommands},
						 .ColorAttachments = {{.Texture = Resources.GBufferBaseColor, .Load = graph::LoadOperation::Clear},
											  {.Texture = Resources.GBufferNormalRoughness, .Load = graph::LoadOperation::Clear},
											  {.Texture = Resources.GBufferMaterial, .Load = graph::LoadOperation::Clear},
											  {.Texture = Resources.Velocity, .Load = graph::LoadOperation::Clear},
											  {.Texture = Resources.ObjectID, .Load = graph::LoadOperation::Clear}},
						 .DepthAttachment = graph::DepthAttachment{.Texture = Resources.Depth, .Load = graph::LoadOperation::Load},
						 .Execute = Wrap(Callbacks.GBuffer)});
	Add("ClusteredLights", graph::PassQueue::Compute, {Resources.Depth}, {}, {}, {Resources.ClusterHeaders, Resources.ClusterIndices},
		Callbacks.ClusteredLights);
	Add("DeferredLighting", graph::PassQueue::Compute,
		{Resources.GBufferBaseColor, Resources.GBufferNormalRoughness, Resources.GBufferMaterial, Resources.Depth,
		 Resources.DirectionalShadowAtlas, Resources.SpotShadowAtlas, Resources.PointShadowArray},
		{Resources.ClusterHeaders, Resources.ClusterIndices}, {Resources.HDRLighting}, {}, Callbacks.DeferredLighting);
	(void)Graph.AddPass(
		{.Name = "WeightedOIT",
		 .Queue = graph::PassQueue::Graphics,
		 .ReadTextures = {Resources.Depth},
		 .ReadBuffers = {Inputs.VisibleInstances, Inputs.IndirectCommands, Resources.ClusterHeaders, Resources.ClusterIndices},
		 .ColorAttachments = {{.Texture = Resources.TransparencyAccumulation, .Load = graph::LoadOperation::Clear},
							  {.Texture = Resources.TransparencyRevealage,
							   .Load = graph::LoadOperation::Clear,
							   .ClearColor = glm::vec4(1.0f)}},
		 .DepthAttachment = graph::DepthAttachment{.Texture = Resources.Depth, .Load = graph::LoadOperation::Load},
		 .Execute = Wrap(Callbacks.WeightedOIT)});
	Add("OITComposition", graph::PassQueue::Compute,
		{Resources.HDRLighting, Resources.TransparencyAccumulation, Resources.TransparencyRevealage}, {}, {Resources.CompositedHDR}, {},
		Callbacks.OITComposition);
	Add("TemporalAA", graph::PassQueue::Compute, {Resources.CompositedHDR, Resources.Velocity, Resources.Depth, Resources.TAAHistoryRead},
		{}, {Resources.TAAHistoryWrite, Resources.TAAResolved}, {}, Callbacks.TemporalAA);
	Add("ExposureAndBloom", graph::PassQueue::Compute, {Resources.TAAResolved, Resources.Exposure}, {},
		{Resources.Exposure, Resources.Bloom}, {}, Callbacks.ExposureAndBloom);
	Add("ToneMapAndPresent", graph::PassQueue::Graphics, {Resources.TAAResolved, Resources.Exposure, Resources.Bloom}, {}, {}, {},
		Callbacks.ToneMapAndPresent);
	return *ResourceState;
}
} // namespace renderer
