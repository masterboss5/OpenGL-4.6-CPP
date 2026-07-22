#include "ShaderLibrary.h"

#include "ShaderException.h"

namespace pipeline::shader
{
ShaderLibrary::ShaderLibrary(device::Device &Device, resource::AssetManager &Assets) : Assets(Assets), Device(Device)
{
}
ShaderModule &ShaderLibrary::GetModule(const ShaderSourceAsset &Source, const ShaderPermutationKey &Permutation)
{
	const std::string Key =
		Source.GetSourcePath().generic_string() + ":" + std::to_string(Source.GetSourceHash()) + ":" + Permutation.ToString();
	auto Found = this->Modules.find(Key);
	if (Found != this->Modules.end())
		return *Found->second;
	auto Module = std::make_unique<ShaderModule>(this->Device, Source, Permutation, this->Preprocessor.Preprocess(Source, Permutation));
	ShaderModule &Result = *Module;
	this->Modules.emplace(Key, std::move(Module));
	return Result;
}
std::unique_ptr<GraphicsPipeline> ShaderLibrary::Build(const GraphicsPipelineDescriptor &Descriptor, uint64 &VertexHash,
													   uint64 &FragmentHash)
{
	auto Vertex = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Descriptor.Vertex.Path);
	auto Fragment = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Descriptor.Fragment.Path);
	auto VertexSource = Vertex.Pin();
	auto FragmentSource = Fragment.Pin();
	if (VertexSource->GetStage() != ShaderStage::Vertex || FragmentSource->GetStage() != ShaderStage::Fragment)
		throw ShaderPipelineException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
									  "Pipeline source stage does not match its descriptor");
	VertexHash = VertexSource->GetSourceHash();
	FragmentHash = FragmentSource->GetSourceHash();
	return std::make_unique<GraphicsPipeline>(this->Device, Descriptor, this->GetModule(*VertexSource, Descriptor.Permutation),
											  this->GetModule(*FragmentSource, Descriptor.Permutation));
}
std::unique_ptr<ComputePipeline> ShaderLibrary::Build(const ComputePipelineDescriptor &Descriptor, uint64 &ComputeHash)
{
	auto Compute = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Descriptor.Compute.Path);
	auto ComputeSource = Compute.Pin();
	if (ComputeSource->GetStage() != ShaderStage::Compute)
		throw ShaderPipelineException(ShaderStage::Compute, Descriptor.Compute.Path, Descriptor.Permutation,
									  "Compute source stage does not match its descriptor");
	ComputeHash = ComputeSource->GetSourceHash();
	return std::make_unique<ComputePipeline>(this->Device, Descriptor, this->GetModule(*ComputeSource, Descriptor.Permutation));
}
uint32 ShaderLibrary::CreateGraphicsPipeline(const GraphicsPipelineDescriptor &Descriptor)
{
	PipelineEntry Entry{.Descriptor = Descriptor};
	Entry.Active = this->Build(Entry.Descriptor, Entry.VertexHash, Entry.FragmentHash);
	this->Pipelines.push_back(std::move(Entry));
	return static_cast<uint32>(this->Pipelines.size() - 1);
}
GraphicsPipeline &ShaderLibrary::GetGraphicsPipeline(uint32 PipelineIndex)
{
	if (PipelineIndex >= this->Pipelines.size())
		throw std::out_of_range("Shader pipeline index is out of range");
	return *this->Pipelines[PipelineIndex].Active;
}
uint32 ShaderLibrary::CreateComputePipeline(const ComputePipelineDescriptor &Descriptor)
{
	ComputePipelineEntry Entry{.Descriptor = Descriptor};
	Entry.Active = this->Build(Entry.Descriptor, Entry.ComputeHash);
	this->ComputePipelines.push_back(std::move(Entry));
	return static_cast<uint32>(this->ComputePipelines.size() - 1);
}
ComputePipeline &ShaderLibrary::GetComputePipeline(uint32 PipelineIndex)
{
	if (PipelineIndex >= this->ComputePipelines.size())
		throw std::out_of_range("Compute pipeline index is out of range");
	return *this->ComputePipelines[PipelineIndex].Active;
}
void ShaderLibrary::BeginFrame()
{
	(void)this->Assets.ReloadChangedAssets();
	for (PipelineEntry &Entry : this->Pipelines)
	{
		try
		{
			auto Vertex = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Entry.Descriptor.Vertex.Path);
			auto Fragment = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Entry.Descriptor.Fragment.Path);
			auto VertexSource = Vertex.Pin();
			auto FragmentSource = Fragment.Pin();
			if (VertexSource->GetStage() != ShaderStage::Vertex || FragmentSource->GetStage() != ShaderStage::Fragment)
				throw ShaderPipelineException(ShaderStage::Vertex, Entry.Descriptor.Vertex.Path, Entry.Descriptor.Permutation,
											  "Pipeline source stage does not match its descriptor");
			if (VertexSource->GetSourceHash() == Entry.VertexHash && FragmentSource->GetSourceHash() == Entry.FragmentHash)
				continue;
			uint64 VertexHash = 0;
			uint64 FragmentHash = 0;
			auto Replacement = this->Build(Entry.Descriptor, VertexHash, FragmentHash);
			Entry.Active = std::move(Replacement);
			Entry.VertexHash = VertexHash;
			Entry.FragmentHash = FragmentHash;
		}
		catch (const std::exception &Exception)
		{
			this->LastDiagnostic = Exception.what();
		}
	}
	for (ComputePipelineEntry &Entry : this->ComputePipelines)
	{
		try
		{
			auto Compute = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Entry.Descriptor.Compute.Path);
			auto ComputeSource = Compute.Pin();
			if (ComputeSource->GetStage() != ShaderStage::Compute)
				throw ShaderPipelineException(ShaderStage::Compute, Entry.Descriptor.Compute.Path, Entry.Descriptor.Permutation,
											  "Compute source stage does not match its descriptor");
			if (ComputeSource->GetSourceHash() == Entry.ComputeHash)
				continue;
			uint64 ComputeHash = 0;
			auto Replacement = this->Build(Entry.Descriptor, ComputeHash);
			Entry.Active = std::move(Replacement);
			Entry.ComputeHash = ComputeHash;
		}
		catch (const std::exception &Exception)
		{
			this->LastDiagnostic = Exception.what();
		}
	}
}
const std::string &ShaderLibrary::GetLastDiagnostic() const noexcept
{
	return this->LastDiagnostic;
}
} // namespace pipeline::shader
