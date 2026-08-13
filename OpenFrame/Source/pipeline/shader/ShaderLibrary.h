#pragma once

#include "Source/core/EngineAPI.h"

#include "ComputePipeline.h"
#include "GraphicsPipeline.h"
#include "ShaderPreprocessor.h"
#include "Source/resource/asset/AssetManager.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pipeline::shader
{
class ENGINE_API ShaderLibrary final
{
  public:
	ShaderLibrary(device::Device &Device, resource::AssetManager &Assets);
	ShaderLibrary(const ShaderLibrary &) = delete;
	ShaderLibrary &operator=(const ShaderLibrary &) = delete;
	ShaderLibrary(ShaderLibrary &&) = delete;
	ShaderLibrary &operator=(ShaderLibrary &&) = delete;
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
		std::string VertexModuleKey;
		std::string FragmentModuleKey;
		uint64 VertexGeneration = 0;
		uint64 FragmentGeneration = 0;
	};
	struct ComputePipelineEntry final
	{
		ComputePipelineDescriptor Descriptor;
		std::unique_ptr<ComputePipeline> Active;
		std::string ComputeModuleKey;
		uint64 ComputeGeneration = 0;
	};
	struct RetiredGraphicsPipeline final
	{
		std::unique_ptr<GraphicsPipeline> Pipeline;
		std::string VertexModuleKey;
		std::string FragmentModuleKey;
		uint8 FramesRemaining = 0;
	};
	struct RetiredComputePipeline final
	{
		std::unique_ptr<ComputePipeline> Pipeline;
		std::string ComputeModuleKey;
		uint8 FramesRemaining = 0;
	};
	static constexpr uint8 PipelineRetirementFrameCount = 3;
	resource::AssetManager &Assets;
	device::Device &Device;
	ShaderPreprocessor Preprocessor;
	std::unordered_map<std::string, std::shared_ptr<const ShaderModule>> Modules;
	std::unordered_map<std::string, uint32> GraphicsPipelineCache;
	std::unordered_map<std::string, uint32> ComputePipelineCache;
	std::vector<PipelineEntry> Pipelines;
	std::vector<ComputePipelineEntry> ComputePipelines;
	std::vector<const std::string *> ActiveModuleKeys;
	// Replacements stay alive for the three in-flight frame slots before their
	// GL objects and module programs become eligible for destruction.
	std::vector<RetiredGraphicsPipeline> RetiredGraphicsPipelines;
	std::vector<RetiredComputePipeline> RetiredComputePipelines;
	std::string LastDiagnostic;
	[[nodiscard]] std::shared_ptr<const ShaderModule> GetModule(const ShaderSourceAsset &Source, uint64 SourceGeneration,
																const ShaderPermutationKey &Permutation, std::string *ModuleKey);
	[[nodiscard]] std::unique_ptr<GraphicsPipeline> Build(const GraphicsPipelineDescriptor &Descriptor, uint64 &VertexGeneration,
														  uint64 &FragmentGeneration, std::string &VertexModuleKey,
														  std::string &FragmentModuleKey);
	[[nodiscard]] std::unique_ptr<ComputePipeline> Build(const ComputePipelineDescriptor &Descriptor, uint64 &ComputeGeneration,
														 std::string &ComputeModuleKey);
	void CollectRetiredPipelines();
	void TrimModuleCache();
};
} // namespace pipeline::shader
