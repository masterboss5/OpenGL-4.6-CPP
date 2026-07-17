#pragma once

#include <memory>
#include <unordered_map>

#include "GraphicsPipeline.h"
#include "ComputePipeline.h"
#include "ShaderPreprocessor.h"
#include "src/resource/asset/AssetManager.h"

namespace pipeline::shader
{
	class ShaderLibrary final
	{
	public:
		explicit ShaderLibrary(resource::AssetManager& assets);
		[[nodiscard]] uint32 createGraphicsPipeline(const GraphicsPipelineDescriptor& descriptor);
		[[nodiscard]] GraphicsPipeline& getGraphicsPipeline(uint32 pipelineIndex);
		[[nodiscard]] uint32 createComputePipeline(const ComputePipelineDescriptor& descriptor);
		[[nodiscard]] ComputePipeline& getComputePipeline(uint32 pipelineIndex);
		void beginFrame();
		[[nodiscard]] const std::string& getLastDiagnostic() const noexcept;
	private:
		struct PipelineEntry final { GraphicsPipelineDescriptor descriptor; std::unique_ptr<GraphicsPipeline> active; uint64 vertexHash = 0; uint64 fragmentHash = 0; };
		struct ComputePipelineEntry final { ComputePipelineDescriptor descriptor; std::unique_ptr<ComputePipeline> active; uint64 computeHash = 0; };
		resource::AssetManager& assets;
		ShaderPreprocessor preprocessor;
		std::unordered_map<std::string, std::unique_ptr<ShaderModule>> modules;
		std::vector<PipelineEntry> pipelines;
		std::vector<ComputePipelineEntry> computePipelines;
		std::string lastDiagnostic;
		[[nodiscard]] ShaderModule& getModule(const ShaderSourceAsset& source, const ShaderPermutationKey& permutation);
		[[nodiscard]] std::unique_ptr<GraphicsPipeline> build(const GraphicsPipelineDescriptor& descriptor, uint64& vertexHash, uint64& fragmentHash);
		[[nodiscard]] std::unique_ptr<ComputePipeline> build(const ComputePipelineDescriptor& descriptor, uint64& computeHash);
	};
}
