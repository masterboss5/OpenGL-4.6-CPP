#pragma once

#include "src/pipeline/shader/ComputePipeline.h"
#include "src/pipeline/shader/GraphicsPipeline.h"

namespace pipeline::render
{
// Non-owning frame-pipeline contract. ShaderLibrary retains every pipeline;
// Renderer only schedules them into the graph for the current frame.
struct RenderPassPipelineSet final
{
	const pipeline::shader::GraphicsPipeline &ShadowDepth;
	const pipeline::shader::GraphicsPipeline &DepthPrepass;
	const pipeline::shader::GraphicsPipeline &GBuffer;
	const pipeline::shader::GraphicsPipeline &TransparentOIT;
	const pipeline::shader::GraphicsPipeline &AccuratePicking;
	const pipeline::shader::GraphicsPipeline &EditorWireframe;
	const pipeline::shader::GraphicsPipeline &EditorGrid;
	const pipeline::shader::GraphicsPipeline &EditorDebugLine;
	const pipeline::shader::GraphicsPipeline &GizmoOverlay;
	const pipeline::shader::GraphicsPipeline &ToneMap;
	const pipeline::shader::ComputePipeline &VisibilityCull;
	const pipeline::shader::ComputePipeline &VisibilityPrefixScan;
	const pipeline::shader::ComputePipeline &VisibilityBlockPrefixScan;
	const pipeline::shader::ComputePipeline &VisibilityFinalize;
	const pipeline::shader::ComputePipeline &VisibilityScatter;
	const pipeline::shader::ComputePipeline &HierarchicalDepth;
	const pipeline::shader::ComputePipeline &ClusteredLights;
	const pipeline::shader::ComputePipeline &DeferredLighting;
	const pipeline::shader::ComputePipeline &OITComposition;
	const pipeline::shader::ComputePipeline &TemporalAA;
	const pipeline::shader::ComputePipeline &ViewportVisualization;
	const pipeline::shader::ComputePipeline &SelectionOutline;
	const pipeline::shader::ComputePipeline &AutoExposure;
	const pipeline::shader::ComputePipeline &Bloom;
};
} // namespace pipeline::render
