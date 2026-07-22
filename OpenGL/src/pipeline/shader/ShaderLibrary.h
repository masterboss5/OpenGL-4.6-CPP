#pragma once

#include "ComputePipeline.h"
#include "GraphicsPipeline.h"
#include "ShaderPreprocessor.h"
#include "src/resource/asset/AssetManager.h"

#include <memory>
#include <unordered_map>

namespace pipeline::shader
{
class ShaderLibrary final
{
  public:
	ShaderLibrary(device::Device &Device, resource::AssetManager &Assets);
	[[nodiscard]] uint32 CreateGraphicsPipeline(const GraphicsPipelineDescriptor &Descriptor);
	[[nodiscard]] GraphicsPipeline &GetGraphicsPipeline(uint32 PipelineIndex);
	[[nodiscard]] uint32 CreateComputePipeline(const ComputePipelineDescriptor &Descriptor);
	[[nodiscard]] ComputePipeline &GetComputePipeline(uint32 PipelineIndex);
	void BeginFrame();
	[[nodiscard]] const std::string &GetLastDiagnostic() const noexcept;

  private:
	struct PipelineEntry final
	{
		GraphicsPipelineDescriptor Descriptor;
		std::unique_ptr<GraphicsPipeline> Active;
		uint64 VertexHash = 0;
		uint64 FragmentHash = 0;
	};
	struct ComputePipelineEntry final
	{
		ComputePipelineDescriptor Descriptor;
		std::unique_ptr<ComputePipeline> Active;
		uint64 ComputeHash = 0;
	};
	resource::AssetManager &Assets;
	device::Device &Device;
	ShaderPreprocessor Preprocessor;
	std::unordered_map<std::string, std::unique_ptr<ShaderModule>> Modules;
	std::vector<PipelineEntry> Pipelines;
	std::vector<ComputePipelineEntry> ComputePipelines;
	std::string LastDiagnostic;
	[[nodiscard]] ShaderModule &GetModule(const ShaderSourceAsset &Source, const ShaderPermutationKey &Permutation);
	[[nodiscard]] std::unique_ptr<GraphicsPipeline> Build(const GraphicsPipelineDescriptor &Descriptor, uint64 &VertexHash,
														  uint64 &FragmentHash);
	[[nodiscard]] std::unique_ptr<ComputePipeline> Build(const ComputePipelineDescriptor &Descriptor, uint64 &ComputeHash);
};
} // namespace pipeline::shader
