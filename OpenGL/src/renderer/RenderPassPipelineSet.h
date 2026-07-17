#pragma once

#include "src/pipeline/shader/ComputePipeline.h"
#include "src/pipeline/shader/GraphicsPipeline.h"

namespace renderer
{
	// Non-owning frame-pipeline contract. ShaderLibrary retains every pipeline;
	// OpenGLRenderer only schedules them into the graph for the current frame.
	struct RenderPassPipelineSet final
	{
		const pipeline::shader::GraphicsPipeline& shadowDepth;
		const pipeline::shader::GraphicsPipeline& depthPrepass;
		const pipeline::shader::GraphicsPipeline& gbuffer;
		const pipeline::shader::GraphicsPipeline& transparentOIT;
		const pipeline::shader::GraphicsPipeline& toneMap;
		const pipeline::shader::ComputePipeline& visibilityCull;
		const pipeline::shader::ComputePipeline& visibilityPrefixScan;
		const pipeline::shader::ComputePipeline& visibilityBlockPrefixScan;
		const pipeline::shader::ComputePipeline& visibilityFinalize;
		const pipeline::shader::ComputePipeline& visibilityScatter;
		const pipeline::shader::ComputePipeline& hierarchicalDepth;
		const pipeline::shader::ComputePipeline& clusteredLights;
		const pipeline::shader::ComputePipeline& deferredLighting;
		const pipeline::shader::ComputePipeline& oitComposition;
		const pipeline::shader::ComputePipeline& temporalAA;
		const pipeline::shader::ComputePipeline& autoExposure;
		const pipeline::shader::ComputePipeline& bloom;
	};
}
