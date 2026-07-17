#include "ShaderLibrary.h"

#include "ShaderException.h"

namespace pipeline::shader
{
	ShaderLibrary::ShaderLibrary(resource::AssetManager& assets) : assets(assets) {}
	ShaderModule& ShaderLibrary::getModule(const ShaderSourceAsset& source, const ShaderPermutationKey& permutation)
	{
		const std::string key = source.getSourcePath().generic_string() + ":" + std::to_string(source.getSourceHash()) + ":" + permutation.toString();
		auto found = this->modules.find(key); if (found != this->modules.end()) return *found->second;
		auto module = std::make_unique<ShaderModule>(source, permutation, this->preprocessor.preprocess(source, permutation)); ShaderModule& result = *module; this->modules.emplace(key, std::move(module)); return result;
	}
	std::unique_ptr<GraphicsPipeline> ShaderLibrary::build(const GraphicsPipelineDescriptor& descriptor, uint64& vertexHash, uint64& fragmentHash)
	{
		auto vertex = this->assets.getAsset<ShaderSourceAsset>(resource::AssetType::SHADER_SOURCE, descriptor.vertex.path);
		auto fragment = this->assets.getAsset<ShaderSourceAsset>(resource::AssetType::SHADER_SOURCE, descriptor.fragment.path);
		if (!vertex || !fragment || vertex->getStage() != ShaderStage::Vertex || fragment->getStage() != ShaderStage::Fragment) throw ShaderPipelineException(ShaderStage::Vertex, descriptor.vertex.path, descriptor.permutation, "Pipeline source stage does not match its descriptor");
		vertexHash = vertex->getSourceHash(); fragmentHash = fragment->getSourceHash(); return std::make_unique<GraphicsPipeline>(descriptor, this->getModule(*vertex, descriptor.permutation), this->getModule(*fragment, descriptor.permutation));
	}
	std::unique_ptr<ComputePipeline> ShaderLibrary::build(const ComputePipelineDescriptor& descriptor, uint64& computeHash)
	{
		auto compute = this->assets.getAsset<ShaderSourceAsset>(resource::AssetType::SHADER_SOURCE, descriptor.compute.path);
		if (!compute || compute->getStage() != ShaderStage::Compute) throw ShaderPipelineException(ShaderStage::Compute, descriptor.compute.path, descriptor.permutation, "Compute source stage does not match its descriptor");
		computeHash = compute->getSourceHash();
		return std::make_unique<ComputePipeline>(descriptor, this->getModule(*compute, descriptor.permutation));
	}
	uint32 ShaderLibrary::createGraphicsPipeline(const GraphicsPipelineDescriptor& descriptor)
	{
		PipelineEntry entry { .descriptor = descriptor }; entry.active = this->build(entry.descriptor, entry.vertexHash, entry.fragmentHash); this->pipelines.push_back(std::move(entry)); return static_cast<uint32>(this->pipelines.size() - 1);
	}
	GraphicsPipeline& ShaderLibrary::getGraphicsPipeline(uint32 pipelineIndex) { if (pipelineIndex >= this->pipelines.size()) throw std::out_of_range("Shader pipeline index is out of range"); return *this->pipelines[pipelineIndex].active; }
	uint32 ShaderLibrary::createComputePipeline(const ComputePipelineDescriptor& descriptor)
	{
		ComputePipelineEntry entry { .descriptor = descriptor };
		entry.active = this->build(entry.descriptor, entry.computeHash);
		this->computePipelines.push_back(std::move(entry));
		return static_cast<uint32>(this->computePipelines.size() - 1);
	}
	ComputePipeline& ShaderLibrary::getComputePipeline(uint32 pipelineIndex) { if (pipelineIndex >= this->computePipelines.size()) throw std::out_of_range("Compute pipeline index is out of range"); return *this->computePipelines[pipelineIndex].active; }
	void ShaderLibrary::beginFrame()
	{
		(void)this->assets.reloadChangedAssets();
		for (PipelineEntry& entry : this->pipelines)
		{
			try { uint64 vertexHash = 0; uint64 fragmentHash = 0; auto replacement = this->build(entry.descriptor, vertexHash, fragmentHash); if (vertexHash != entry.vertexHash || fragmentHash != entry.fragmentHash) { entry.active = std::move(replacement); entry.vertexHash = vertexHash; entry.fragmentHash = fragmentHash; } }
			catch (const std::exception& exception) { this->lastDiagnostic = exception.what(); }
		}
		for (ComputePipelineEntry& entry : this->computePipelines)
		{
			try { uint64 computeHash = 0; auto replacement = this->build(entry.descriptor, computeHash); if (computeHash != entry.computeHash) { entry.active = std::move(replacement); entry.computeHash = computeHash; } }
			catch (const std::exception& exception) { this->lastDiagnostic = exception.what(); }
		}
	}
	const std::string& ShaderLibrary::getLastDiagnostic() const noexcept { return this->lastDiagnostic; }
}
