#include "HybridDeferredFrameGraph.h"

#include "Source/pipeline/render/RenderData.h"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace pipeline::graph
{
uint32 HybridDeferredFrameGraph::CalculateMipCount(Extent2D Extent)
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

HybridDeferredFrameResources HybridDeferredFrameGraph::Build(RenderGraph &Graph, const HybridDeferredFrameInputs &Inputs,
															 const HybridDeferredPassCallbacks &Callbacks) const
{
	HybridDeferredFrameResources Resources;
	this->BuildImpl(Graph, Inputs, Callbacks, Resources);
	return Resources;
}

void HybridDeferredFrameGraph::BuildInto(RenderGraph &Graph, const HybridDeferredFrameInputs &Inputs,
										 const HybridDeferredPassCallbacks &Callbacks, HybridDeferredFrameResources &Resources) const
{
	this->BuildImpl(Graph, Inputs, Callbacks, Resources);
}

void HybridDeferredFrameGraph::BuildImpl(RenderGraph &Graph, const HybridDeferredFrameInputs &Inputs,
										 const HybridDeferredPassCallbacks &Callbacks, HybridDeferredFrameResources &Resources) const
{
	if (!Inputs.Extent.IsValid())
		throw std::invalid_argument("Hybrid deferred frame graph requires a valid extent");
	if (Inputs.SelectionMaskWordCount == 0)
		throw std::invalid_argument("Hybrid deferred frame graph requires a non-empty selection-mask buffer");
	if (Inputs.HistoryNamespace.empty())
		throw std::invalid_argument("Hybrid deferred frame graph requires a persistent-history namespace");
	RequireCallback(Callbacks.DirectionalShadows, "DirectionalShadows");
	RequireCallback(Callbacks.SpotShadows, "SpotShadows");
	RequireCallback(Callbacks.PointShadows, "PointShadows");
	RequireCallback(Callbacks.MainVisibility, "MainVisibility");
	if (Inputs.AccuratePickingEnabled)
		RequireCallback(Callbacks.AccuratePicking, "AccuratePicking");
	RequireCallback(Callbacks.DepthPrepass, "DepthPrepass");
	RequireCallback(Callbacks.HierarchicalDepth, "HierarchicalDepth");
	RequireCallback(Callbacks.GBuffer, "GBuffer");
	RequireCallback(Callbacks.ClusteredLights, "ClusteredLights");
	RequireCallback(Callbacks.DeferredLighting, "DeferredLighting");
	RequireCallback(Callbacks.WeightedOIT, "WeightedOIT");
	RequireCallback(Callbacks.OITComposition, "OITComposition");
	RequireCallback(Callbacks.TemporalAA, "TemporalAA");
	RequireCallback(Callbacks.ViewportVisualization, "ViewportVisualization");
	RequireCallback(Callbacks.SelectionOutline, "SelectionOutline");
	RequireCallback(Callbacks.WireframeOverlay, "WireframeOverlay");
	RequireCallback(Callbacks.EditorGrid, "EditorGrid");
	RequireCallback(Callbacks.DebugOverlay, "DebugOverlay");
	RequireCallback(Callbacks.GizmoOverlay, "GizmoOverlay");
	RequireCallback(Callbacks.ExposureAndBloom, "ExposureAndBloom");
	RequireCallback(Callbacks.ToneMapAndPresent, "ToneMapAndPresent");
	if (Inputs.DirectionalShadowLayerCount > pipeline::render::DirectionalShadowCascadeCount ||
		Inputs.SpotShadowLayerCount > pipeline::render::MaximumSpotShadowCount ||
		Inputs.PointShadowFaceLayerCount > pipeline::render::MaximumPointShadowFaceCount || Inputs.PointShadowFaceLayerCount % 6U != 0U)
		throw std::invalid_argument("Hybrid deferred shadow layer counts exceed the renderer shadow budget");
	if (Inputs.DirectionalShadowResolution < pipeline::render::MinimumDirectionalShadowResolution ||
		Inputs.DirectionalShadowResolution > pipeline::render::MaximumDirectionalShadowResolution ||
		(Inputs.DirectionalShadowResolution & (Inputs.DirectionalShadowResolution - 1U)) != 0U)
		throw std::invalid_argument("Directional shadow resolution must be a power of two from 256 through 8192");
	const uint64 DirectionalShadowBytes = static_cast<uint64>(Inputs.DirectionalShadowResolution) * Inputs.DirectionalShadowResolution *
										  std::max(Inputs.DirectionalShadowLayerCount, 1U) * sizeof(float32);
	if (DirectionalShadowBytes > pipeline::render::DirectionalShadowMemoryBudgetBytes)
		throw std::invalid_argument("Directional shadow allocation exceeds the 512 MiB renderer budget");

	const Extent2D DirectionalShadowExtent{Inputs.DirectionalShadowResolution, Inputs.DirectionalShadowResolution};
	const uint32 DirectionalShadowLayers = std::max(Inputs.DirectionalShadowLayerCount, 1U);
	const uint32 SpotShadowLayers = std::max(Inputs.SpotShadowLayerCount, 1U);
	const uint32 PointShadowFaceLayers = std::max(Inputs.PointShadowFaceLayerCount, 6U);
	const uint32 EffectiveSpotShadowResolution = pipeline::render::CalculateSpotShadowResolution(SpotShadowLayers);
	const uint32 EffectivePointShadowResolution = pipeline::render::CalculatePointShadowResolution(PointShadowFaceLayers);
	const Extent2D SpotShadowExtent{EffectiveSpotShadowResolution, EffectiveSpotShadowResolution};
	const Extent2D PointShadowExtent{EffectivePointShadowResolution, EffectivePointShadowResolution};
	const uint32 HierarchicalMipCount = CalculateMipCount(Inputs.Extent);
	// Each RenderGraph belongs to one RenderView, so persistent resource names are
	// scoped by the graph and remain stable without rebuilding a per-frame namespace string.
	const TextureHandle TAAHistoryA =
		Graph.CreateTexture("TAAHistoryA", Inputs.Extent, TextureFormat::RGBA16Float, TextureDimension::Texture2D, 1, 1, 1, true);
	const TextureHandle TAAHistoryB =
		Graph.CreateTexture("TAAHistoryB", Inputs.Extent, TextureFormat::RGBA16Float, TextureDimension::Texture2D, 1, 1, 1, true);
	const TextureHandle TAAObjectIDHistoryA = Graph.CreateTexture("TAAObjectIDHistoryA", Inputs.Extent, TextureFormat::R32UnsignedInteger,
																  TextureDimension::Texture2D, 1, 1, 1, true);
	const TextureHandle TAAObjectIDHistoryB = Graph.CreateTexture("TAAObjectIDHistoryB", Inputs.Extent, TextureFormat::R32UnsignedInteger,
																  TextureDimension::Texture2D, 1, 1, 1, true);
	const bool WriteHistoryA = (Inputs.TemporalHistoryWriteIndex & 1U) == 0U;
	const TextureHandle HierarchicalDepthA = Graph.CreateTexture("HierarchicalDepthA", Inputs.Extent, TextureFormat::R32Float,
																 TextureDimension::Texture2D, HierarchicalMipCount, 1, 1, true);
	const TextureHandle HierarchicalDepthB = Graph.CreateTexture("HierarchicalDepthB", Inputs.Extent, TextureFormat::R32Float,
																 TextureDimension::Texture2D, HierarchicalMipCount, 1, 1, true);
	Resources = HybridDeferredFrameResources{
		.DirectionalShadowAtlas = Graph.CreateTexture("DirectionalShadowAtlas", DirectionalShadowExtent, TextureFormat::Depth32Float,
													  TextureDimension::Texture2DArray, 1, DirectionalShadowLayers, 1, true,
													  TextureSamplingMode::DepthComparison),
		.SpotShadowAtlas =
			Graph.CreateTexture("SpotShadowAtlas", SpotShadowExtent, TextureFormat::Depth32Float, TextureDimension::Texture2DArray, 1,
								SpotShadowLayers, 1, true, TextureSamplingMode::DepthComparison),
		.PointShadowArray =
			Graph.CreateTexture("PointShadowArray", PointShadowExtent, TextureFormat::Depth32Float, TextureDimension::TextureCubeArray, 1,
								PointShadowFaceLayers, 1, true, TextureSamplingMode::DepthComparison),
		.Depth = Graph.CreateTexture("MainDepth", Inputs.Extent, TextureFormat::Depth32Float),
		.HierarchicalDepthRead = WriteHistoryA ? HierarchicalDepthB : HierarchicalDepthA,
		.HierarchicalDepthWrite = WriteHistoryA ? HierarchicalDepthA : HierarchicalDepthB,
		.GBufferBaseColor = Graph.CreateTexture("GBufferBaseColor", Inputs.Extent, TextureFormat::RGBA16Float),
		.GBufferNormalRoughness = Graph.CreateTexture("GBufferNormalRoughness", Inputs.Extent, TextureFormat::RGBA16Float),
		.GBufferMaterial = Graph.CreateTexture("GBufferMaterial", Inputs.Extent, TextureFormat::RGBA16Float),
		.GBufferOcclusion = Graph.CreateTexture("GBufferOcclusion", Inputs.Extent, TextureFormat::R8Unorm),
		.Velocity = Graph.CreateTexture("MotionVectors", Inputs.Extent, TextureFormat::RG16Float),
		.ObjectID = Graph.CreateTexture("ObjectID", Inputs.Extent, TextureFormat::R32UnsignedInteger),
		.Overdraw = Graph.CreateTexture("Overdraw", Inputs.Extent, TextureFormat::R32UnsignedInteger),
		.HDRLighting = Graph.CreateTexture("HDRLighting", Inputs.Extent, TextureFormat::RGBA16Float),
		.TransparencyAccumulation = Graph.CreateTexture("OITAccumulation", Inputs.Extent, TextureFormat::RGBA16Float),
		.TransparencyRevealage = Graph.CreateTexture("OITRevealage", Inputs.Extent, TextureFormat::R32Float),
		.CompositedHDR = Graph.CreateTexture("CompositedHDR", Inputs.Extent, TextureFormat::RGBA16Float),
		.TAAHistoryRead = WriteHistoryA ? TAAHistoryB : TAAHistoryA,
		.TAAHistoryWrite = WriteHistoryA ? TAAHistoryA : TAAHistoryB,
		.TAAObjectIDHistoryRead = WriteHistoryA ? TAAObjectIDHistoryB : TAAObjectIDHistoryA,
		.TAAObjectIDHistoryWrite = WriteHistoryA ? TAAObjectIDHistoryA : TAAObjectIDHistoryB,
		.TAAResolved = Graph.CreateTexture("TAAResolved", Inputs.Extent, TextureFormat::RGBA16Float),
		.VisualizedHDR = Graph.CreateTexture("ViewportVisualization", Inputs.Extent, TextureFormat::RGBA16Float),
		.OutlinedHDR = Graph.CreateTexture("OutlinedHDR", Inputs.Extent, TextureFormat::RGBA16Float),
		.Exposure = Graph.CreateTexture("AutoExposure", {1, 1}, TextureFormat::R32Float, TextureDimension::Texture2D, 1, 1, 1, true),
		.Bloom = Graph.CreateTexture("Bloom", Inputs.Extent, TextureFormat::RGBA16Float, TextureDimension::Texture2D, HierarchicalMipCount),
		.Presentation =
			Graph.CreateTexture("Presentation", Inputs.Extent, TextureFormat::RGBA8SRGB, TextureDimension::Texture2D, 1, 1, 1, true),
		.ClusterHeaders = Graph.CreateBuffer("ClusterHeaders", static_cast<uint64>(sizeof(uint32) * 4U) * pipeline::render::ClusterCount),
		.ClusterIndices = Graph.CreateBuffer("ClusterIndices", static_cast<uint64>(sizeof(uint32)) * pipeline::render::ClusterCount *
																   pipeline::render::MaximumLightsPerCluster)};
	if (Inputs.AccuratePickingEnabled)
	{
		Resources.AccuratePickDepth = Graph.CreateTexture("AccuratePickDepth", Inputs.Extent, TextureFormat::Depth32Float);
		Resources.AccuratePickObjectID = Graph.CreateTexture("AccuratePickObjectID", Inputs.Extent, TextureFormat::R32UnsignedInteger);
	}

	auto Add = [&Graph, &Resources](const string_view Name, const PassQueue Queue, const std::initializer_list<TextureHandle> ReadTextures,
									const std::initializer_list<BufferAccess> ReadBuffers,
									const std::initializer_list<TextureAttachment> ColorAttachments,
									const std::optional<DepthAttachment> Depth, const std::initializer_list<TextureHandle> WriteTextures,
									const std::initializer_list<BufferAccess> WriteBuffers, const HybridDeferredPassCallback &Execute)
	{
		const auto Invocation = [Callback = Execute, ResourceSnapshot = Resources](RenderGraphContext &Context)
		{ Callback(Context, ResourceSnapshot); };
		(void)Graph.AddPass(Name, Queue, ReadTextures, ReadBuffers, ColorAttachments, Depth, WriteTextures, WriteBuffers, Invocation);
	};
	Add("DirectionalShadows", PassQueue::Graphics, {}, {{Inputs.ShadowInstances, BufferUsage::ShaderStorage}}, {},
		DepthAttachment{
			.Texture = Resources.DirectionalShadowAtlas, .Load = LoadOperation::Load, .Store = StoreOperation::Store, .ClearDepth = 0.0f},
		{}, {}, Callbacks.DirectionalShadows);
	Add("SpotShadows", PassQueue::Graphics, {}, {{Inputs.ShadowInstances, BufferUsage::ShaderStorage}}, {},
		DepthAttachment{
			.Texture = Resources.SpotShadowAtlas, .Load = LoadOperation::Load, .Store = StoreOperation::Store, .ClearDepth = 0.0f},
		{}, {}, Callbacks.SpotShadows);
	Add("PointShadows", PassQueue::Graphics, {}, {{Inputs.ShadowInstances, BufferUsage::ShaderStorage}}, {},
		DepthAttachment{
			.Texture = Resources.PointShadowArray, .Load = LoadOperation::Load, .Store = StoreOperation::Store, .ClearDepth = 0.0f},
		{}, {}, Callbacks.PointShadows);
	Add("MainVisibility", PassQueue::Compute, {Resources.HierarchicalDepthRead},
		{{Inputs.CandidateInstances, BufferUsage::ShaderStorage}, {Inputs.BatchMetadata, BufferUsage::ShaderStorage}}, {}, std::nullopt, {},
		{{Inputs.VisibleInstances, BufferUsage::ShaderStorage},
		 {Inputs.IndirectCommands, BufferUsage::ShaderStorage | BufferUsage::Indirect},
		 {Inputs.VisibilityScratch, BufferUsage::ShaderStorage}},
		Callbacks.MainVisibility);
	if (Inputs.AccuratePickingEnabled)
	{
		Add("AccuratePicking", PassQueue::Graphics, {},
			{{Inputs.VisibleInstances, BufferUsage::ShaderStorage}, {Inputs.IndirectCommands, BufferUsage::Indirect}},
			{{.Texture = Resources.AccuratePickObjectID, .Load = LoadOperation::Discard, .Store = StoreOperation::Store}},
			DepthAttachment{.Texture = Resources.AccuratePickDepth,
							.Load = LoadOperation::Discard,
							.Store = StoreOperation::Discard,
							.ClearDepth = 0.0f},
			{}, {}, Callbacks.AccuratePicking);
	}
	Add("DepthPrepass", PassQueue::Graphics, {},
		{{Inputs.VisibleInstances, BufferUsage::ShaderStorage}, {Inputs.IndirectCommands, BufferUsage::Indirect}}, {},
		DepthAttachment{.Texture = Resources.Depth, .Load = LoadOperation::Clear, .Store = StoreOperation::Store, .ClearDepth = 0.0f}, {},
		{}, Callbacks.DepthPrepass);
	Add("HierarchicalDepth", PassQueue::Compute, {Resources.Depth}, {}, {}, std::nullopt, {Resources.HierarchicalDepthWrite}, {},
		Callbacks.HierarchicalDepth);
	Add("GBuffer", PassQueue::Graphics, {},
		{{Inputs.VisibleInstances, BufferUsage::ShaderStorage}, {Inputs.IndirectCommands, BufferUsage::Indirect}},
		{{.Texture = Resources.GBufferBaseColor, .Load = LoadOperation::Clear},
		 {.Texture = Resources.GBufferNormalRoughness, .Load = LoadOperation::Clear},
		 {.Texture = Resources.GBufferMaterial, .Load = LoadOperation::Clear},
		 {.Texture = Resources.Velocity, .Load = LoadOperation::Clear},
		 {.Texture = Resources.ObjectID, .Load = LoadOperation::Clear},
		 {.Texture = Resources.GBufferOcclusion, .Load = LoadOperation::Clear, .ClearColor = glm::vec4(1.0f)}},
		DepthAttachment{.Texture = Resources.Depth, .Load = LoadOperation::Load}, {Resources.Overdraw}, {}, Callbacks.GBuffer);
	Add("ClusteredLights", PassQueue::Compute, {Resources.Depth}, {}, {}, std::nullopt, {},
		{{Resources.ClusterHeaders, BufferUsage::ShaderStorage}, {Resources.ClusterIndices, BufferUsage::ShaderStorage}},
		Callbacks.ClusteredLights);
	Add("DeferredLighting", PassQueue::Compute,
		{Resources.GBufferBaseColor, Resources.GBufferNormalRoughness, Resources.GBufferMaterial, Resources.GBufferOcclusion,
		 Resources.Depth, Resources.DirectionalShadowAtlas, Resources.SpotShadowAtlas, Resources.PointShadowArray},
		{{Resources.ClusterHeaders, BufferUsage::ShaderStorage}, {Resources.ClusterIndices, BufferUsage::ShaderStorage}}, {}, std::nullopt,
		{Resources.HDRLighting}, {}, Callbacks.DeferredLighting);
	Add("WeightedOIT", PassQueue::Graphics,
		{Resources.Depth, Resources.HDRLighting, Resources.DirectionalShadowAtlas, Resources.SpotShadowAtlas, Resources.PointShadowArray,
		 Resources.Overdraw},
		{{Inputs.VisibleInstances, BufferUsage::ShaderStorage},
		 {Inputs.IndirectCommands, BufferUsage::Indirect},
		 {Resources.ClusterHeaders, BufferUsage::ShaderStorage},
		 {Resources.ClusterIndices, BufferUsage::ShaderStorage}},
		{{.Texture = Resources.TransparencyAccumulation, .Load = LoadOperation::Clear},
		 {.Texture = Resources.TransparencyRevealage, .Load = LoadOperation::Clear, .ClearColor = glm::vec4(1.0f)}},
		DepthAttachment{.Texture = Resources.Depth, .Load = LoadOperation::Load}, {Resources.Overdraw}, {}, Callbacks.WeightedOIT);
	Add("OITComposition", PassQueue::Compute, {Resources.HDRLighting, Resources.TransparencyAccumulation, Resources.TransparencyRevealage},
		{}, {}, std::nullopt, {Resources.CompositedHDR}, {}, Callbacks.OITComposition);
	Add("TemporalAA", PassQueue::Compute,
		{Resources.CompositedHDR, Resources.Velocity, Resources.Depth, Resources.ObjectID, Resources.TAAHistoryRead,
		 Resources.TAAObjectIDHistoryRead},
		{}, {}, std::nullopt, {Resources.TAAHistoryWrite, Resources.TAAObjectIDHistoryWrite, Resources.TAAResolved}, {},
		Callbacks.TemporalAA);
	Add("ViewportVisualization", PassQueue::Compute,
		{Resources.TAAResolved, Resources.GBufferBaseColor, Resources.GBufferNormalRoughness, Resources.GBufferMaterial, Resources.Depth,
		 Resources.ObjectID, Resources.Overdraw},
		{}, {}, std::nullopt, {Resources.VisualizedHDR}, {}, Callbacks.ViewportVisualization);
	Add("SelectionOutline", PassQueue::Compute, {Resources.VisualizedHDR, Resources.ObjectID},
		{{Inputs.SelectionMask, BufferUsage::ShaderStorage}}, {}, std::nullopt, {Resources.OutlinedHDR}, {}, Callbacks.SelectionOutline);
	Add("WireframeOverlay", PassQueue::Graphics, {},
		{{Inputs.VisibleInstances, BufferUsage::ShaderStorage}, {Inputs.IndirectCommands, BufferUsage::Indirect}},
		{{.Texture = Resources.OutlinedHDR, .Load = LoadOperation::Load, .Store = StoreOperation::Store}},
		DepthAttachment{.Texture = Resources.Depth, .Load = LoadOperation::Load}, {}, {}, Callbacks.WireframeOverlay);
	Add("EditorGrid", PassQueue::Graphics, {}, {},
		{{.Texture = Resources.OutlinedHDR, .Load = LoadOperation::Load, .Store = StoreOperation::Store}},
		DepthAttachment{.Texture = Resources.Depth, .Load = LoadOperation::Load}, {}, {}, Callbacks.EditorGrid);
	Add("DebugOverlay", PassQueue::Graphics, {}, {},
		{{.Texture = Resources.OutlinedHDR, .Load = LoadOperation::Load, .Store = StoreOperation::Store}},
		DepthAttachment{.Texture = Resources.Depth, .Load = LoadOperation::Load}, {}, {}, Callbacks.DebugOverlay);
	Add("GizmoOverlay", PassQueue::Graphics, {}, {},
		{{.Texture = Resources.OutlinedHDR, .Load = LoadOperation::Load, .Store = StoreOperation::Store}}, std::nullopt, {}, {},
		Callbacks.GizmoOverlay);
	Add("ExposureAndBloom", PassQueue::Compute, {Resources.OutlinedHDR, Resources.Exposure}, {}, {}, std::nullopt,
		{Resources.Exposure, Resources.Bloom}, {}, Callbacks.ExposureAndBloom);
	Add("ToneMapAndPresent", PassQueue::Graphics, {Resources.OutlinedHDR, Resources.Exposure, Resources.Bloom}, {},
		{{.Texture = Resources.Presentation, .Load = LoadOperation::Discard, .Store = StoreOperation::Store}}, std::nullopt, {}, {},
		Callbacks.ToneMapAndPresent);
}
} // namespace pipeline::graph
