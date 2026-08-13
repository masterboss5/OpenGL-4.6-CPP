#include "ShaderLibrary.h"

#include "ShaderException.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace
{
void AppendKeyValue(std::string &Key, const uint64 Value)
{
	Key += std::to_string(Value);
	Key.push_back('|');
}

void AppendKeyString(std::string &Key, const std::string &Value)
{
	AppendKeyValue(Key, static_cast<uint64>(Value.size()));
	Key += Value;
	Key.push_back('|');
}

void AppendKeyBool(std::string &Key, const bool Value)
{
	AppendKeyValue(Key, Value ? 1U : 0U);
}

void AppendKeyBlendState(std::string &Key, const pipeline::shader::BlendState &State)
{
	AppendKeyBool(Key, State.Enabled);
	AppendKeyValue(Key, static_cast<uint32>(State.SourceColor));
	AppendKeyValue(Key, static_cast<uint32>(State.DestinationColor));
	AppendKeyValue(Key, static_cast<uint32>(State.SourceAlpha));
	AppendKeyValue(Key, static_cast<uint32>(State.DestinationAlpha));
	AppendKeyValue(Key, static_cast<uint32>(State.ColorOperation));
	AppendKeyValue(Key, static_cast<uint32>(State.AlphaOperation));
}

void AppendKeyStencilFace(std::string &Key, const pipeline::shader::StencilFaceState &State)
{
	AppendKeyValue(Key, static_cast<uint32>(State.Compare));
	AppendKeyValue(Key, static_cast<uint32>(State.Fail));
	AppendKeyValue(Key, static_cast<uint32>(State.DepthFail));
	AppendKeyValue(Key, static_cast<uint32>(State.Pass));
	AppendKeyValue(Key, State.Reference);
	AppendKeyValue(Key, State.ReadMask);
	AppendKeyValue(Key, State.WriteMask);
}

void AppendKeyResourceContracts(std::string &Key, const std::vector<pipeline::shader::ShaderResourceContract> &Contracts)
{
	AppendKeyValue(Key, static_cast<uint64>(Contracts.size()));
	for (const pipeline::shader::ShaderResourceContract &Contract : Contracts)
	{
		AppendKeyString(Key, Contract.Name);
		AppendKeyValue(Key, static_cast<uint32>(Contract.ResourceClass));
		AppendKeyValue(Key, Contract.Type);
		AppendKeyValue(Key, static_cast<uint64>(static_cast<int64>(Contract.Binding)));
		AppendKeyValue(Key, static_cast<uint64>(static_cast<int64>(Contract.ArraySize)));
	}
}

[[nodiscard]] std::string MakeGraphicsPipelineKey(const pipeline::shader::GraphicsPipelineDescriptor &Descriptor,
												  const resource::AssetManager &Assets)
{
	std::string Key;
	Key.reserve(512);
	AppendKeyString(Key, Assets.ResolvePath(Descriptor.Vertex.Path).generic_string());
	AppendKeyValue(Key, static_cast<uint32>(Descriptor.Vertex.Stage));
	AppendKeyString(Key, Assets.ResolvePath(Descriptor.Fragment.Path).generic_string());
	AppendKeyValue(Key, static_cast<uint32>(Descriptor.Fragment.Stage));
	AppendKeyValue(Key, Descriptor.Permutation.GetMask());
	AppendKeyValue(Key, Descriptor.RequiredVertexUniforms);
	AppendKeyValue(Key, Descriptor.RequiredFragmentUniforms);
	AppendKeyValue(Key, Descriptor.RequiredVertexEngineInterface);
	AppendKeyValue(Key, Descriptor.RequiredFragmentEngineInterface);
	AppendKeyResourceContracts(Key, Descriptor.RequiredVertexResources);
	AppendKeyResourceContracts(Key, Descriptor.RequiredFragmentResources);

	AppendKeyValue(Key, static_cast<uint32>(Descriptor.State.Rasterizer.CullMode));
	AppendKeyValue(Key, static_cast<uint32>(Descriptor.State.Rasterizer.FrontFace));
	AppendKeyBool(Key, Descriptor.State.Rasterizer.Wireframe);
	AppendKeyBool(Key, Descriptor.State.DepthStencil.DepthTest);
	AppendKeyBool(Key, Descriptor.State.DepthStencil.DepthWrite);
	AppendKeyValue(Key, static_cast<uint32>(Descriptor.State.DepthStencil.DepthCompare));
	AppendKeyBool(Key, Descriptor.State.DepthStencil.StencilTest);
	AppendKeyStencilFace(Key, Descriptor.State.DepthStencil.FrontStencil);
	AppendKeyStencilFace(Key, Descriptor.State.DepthStencil.BackStencil);
	AppendKeyBlendState(Key, Descriptor.State.Blend);
	AppendKeyValue(Key, static_cast<uint64>(Descriptor.State.ColorAttachmentBlends.size()));
	for (const pipeline::shader::BlendState &Blend : Descriptor.State.ColorAttachmentBlends)
		AppendKeyBlendState(Key, Blend);
	AppendKeyValue(Key, Descriptor.State.Multisample.SampleCount);
	AppendKeyBool(Key, Descriptor.State.Multisample.SampleShading);
	AppendKeyValue(Key, std::bit_cast<uint32>(Descriptor.State.Multisample.MinimumSampleShading));
	AppendKeyBool(Key, Descriptor.State.Multisample.AlphaToCoverage);
	AppendKeyBool(Key, Descriptor.State.Multisample.AlphaToOne);
	AppendKeyValue(Key, static_cast<uint32>(Descriptor.State.Topology));
	AppendKeyValue(Key, Descriptor.State.RenderTargets.ColorAttachmentCount);
	AppendKeyBool(Key, Descriptor.State.RenderTargets.HasDepth);
	AppendKeyValue(Key, Descriptor.State.RenderTargets.SampleCount);
	for (const GLenum Format : Descriptor.State.RenderTargets.ColorFormats)
		AppendKeyValue(Key, Format);
	AppendKeyValue(Key, Descriptor.State.RenderTargets.DepthFormat);
	return Key;
}

[[nodiscard]] std::string MakeComputePipelineKey(const pipeline::shader::ComputePipelineDescriptor &Descriptor,
												 const resource::AssetManager &Assets)
{
	std::string Key;
	Key.reserve(128);
	AppendKeyString(Key, Assets.ResolvePath(Descriptor.Compute.Path).generic_string());
	AppendKeyValue(Key, static_cast<uint32>(Descriptor.Compute.Stage));
	AppendKeyValue(Key, Descriptor.Permutation.GetMask());
	AppendKeyValue(Key, Descriptor.RequiredUniforms);
	AppendKeyValue(Key, Descriptor.RequiredEngineInterface);
	AppendKeyResourceContracts(Key, Descriptor.RequiredResources);
	return Key;
}

[[nodiscard]] std::string MakeShaderModuleKey(const pipeline::shader::ShaderSourceAsset &Source, const uint64 SourceGeneration,
											  const pipeline::shader::ShaderPermutationKey &Permutation,
											  const pipeline::shader::ShaderPreprocessResult &Preprocessed)
{
	std::string Key;
	Key.reserve(256 + Preprocessed.Source.size());
	AppendKeyString(Key, Source.GetSourcePath().generic_string());
	AppendKeyValue(Key, static_cast<uint32>(Source.GetStage()));
	AppendKeyValue(Key, SourceGeneration);
	AppendKeyString(Key, Permutation.ToString());
	AppendKeyString(Key, Preprocessed.Source);
	return Key;
}
} // namespace

namespace pipeline::shader
{
ShaderLibrary::ShaderLibrary(device::Device &Device, resource::AssetManager &Assets) : Assets(Assets), Device(Device)
{
	this->Modules.reserve(64);
	this->GraphicsPipelineCache.reserve(16);
	this->ComputePipelineCache.reserve(32);
	this->Pipelines.reserve(16);
	this->ComputePipelines.reserve(32);
	this->ActiveModuleKeys.reserve(64);
	this->RetiredGraphicsPipelines.reserve(16);
	this->RetiredComputePipelines.reserve(32);
}
std::shared_ptr<const ShaderModule> ShaderLibrary::GetModule(const ShaderSourceAsset &Source, const uint64 SourceGeneration,
															 const ShaderPermutationKey &Permutation, std::string *ModuleKey)
{
	ShaderPreprocessResult Preprocessed = this->Preprocessor.Preprocess(Source, Permutation, this->Assets.GetRootPath());
	const std::string Key = MakeShaderModuleKey(Source, SourceGeneration, Permutation, Preprocessed);
	if (ModuleKey != nullptr)
		*ModuleKey = Key;
	auto Found = this->Modules.find(Key);
	if (Found != this->Modules.end())
		return Found->second;
	auto Module = std::make_shared<ShaderModule>(this->Device, Source, Permutation, Preprocessed);
	this->Modules.emplace(Key, Module);
	return Module;
}
std::unique_ptr<GraphicsPipeline> ShaderLibrary::Build(const GraphicsPipelineDescriptor &Descriptor, uint64 &VertexGeneration,
													   uint64 &FragmentGeneration, std::string &VertexModuleKey,
													   std::string &FragmentModuleKey)
{
	auto Vertex = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Descriptor.Vertex.Path);
	auto Fragment = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Descriptor.Fragment.Path);
	auto VertexSource = Vertex.Pin();
	auto FragmentSource = Fragment.Pin();
	if (VertexSource == nullptr || FragmentSource == nullptr)
		throw ShaderPipelineException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
									  "Graphics pipeline source asset is unavailable");
	if (VertexSource->GetStage() != ShaderStage::Vertex || FragmentSource->GetStage() != ShaderStage::Fragment)
		throw ShaderPipelineException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
									  "Pipeline source stage does not match its descriptor");
	VertexGeneration = Vertex.GetPublishedGeneration();
	FragmentGeneration = Fragment.GetPublishedGeneration();
	auto VertexModule = this->GetModule(*VertexSource, VertexGeneration, Descriptor.Permutation, &VertexModuleKey);
	auto FragmentModule = this->GetModule(*FragmentSource, FragmentGeneration, Descriptor.Permutation, &FragmentModuleKey);
	return std::make_unique<GraphicsPipeline>(this->Device, Descriptor, std::move(VertexModule), std::move(FragmentModule));
}
std::unique_ptr<ComputePipeline> ShaderLibrary::Build(const ComputePipelineDescriptor &Descriptor, uint64 &ComputeGeneration,
													  std::string &ComputeModuleKey)
{
	auto Compute = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Descriptor.Compute.Path);
	auto ComputeSource = Compute.Pin();
	if (ComputeSource == nullptr)
		throw ShaderPipelineException(ShaderStage::Compute, Descriptor.Compute.Path, Descriptor.Permutation,
									  "Compute pipeline source asset is unavailable");
	if (ComputeSource->GetStage() != ShaderStage::Compute)
		throw ShaderPipelineException(ShaderStage::Compute, Descriptor.Compute.Path, Descriptor.Permutation,
									  "Compute source stage does not match its descriptor");
	ComputeGeneration = Compute.GetPublishedGeneration();
	auto ComputeModule = this->GetModule(*ComputeSource, ComputeGeneration, Descriptor.Permutation, &ComputeModuleKey);
	return std::make_unique<ComputePipeline>(this->Device, Descriptor, std::move(ComputeModule));
}
uint32 ShaderLibrary::CreateGraphicsPipeline(const GraphicsPipelineDescriptor &Descriptor)
{
	const std::string Key = MakeGraphicsPipelineKey(Descriptor, this->Assets);
	if (const auto Found = this->GraphicsPipelineCache.find(Key); Found != this->GraphicsPipelineCache.end())
	{
		if (Found->second >= this->Pipelines.size())
			throw std::logic_error("Shader graphics-pipeline cache contains an invalid entry");
		return Found->second;
	}
	PipelineEntry Entry{.Descriptor = Descriptor};
	try
	{
		Entry.Active =
			this->Build(Entry.Descriptor, Entry.VertexGeneration, Entry.FragmentGeneration, Entry.VertexModuleKey, Entry.FragmentModuleKey);
	}
	catch (...)
	{
		this->TrimModuleCache();
		throw;
	}
	const usize PreviousPipelineCount = this->Pipelines.size();
	try
	{
		this->Pipelines.push_back(std::move(Entry));
		const uint32 Index = static_cast<uint32>(this->Pipelines.size() - 1);
		const auto [CacheIterator, Inserted] = this->GraphicsPipelineCache.emplace(Key, Index);
		if (!Inserted)
		{
			this->Pipelines.resize(PreviousPipelineCount);
			this->TrimModuleCache();
			if (CacheIterator->second >= this->Pipelines.size())
				throw std::logic_error("Shader graphics-pipeline cache contains an invalid entry");
			return CacheIterator->second;
		}
		return Index;
	}
	catch (...)
	{
		if (this->Pipelines.size() > PreviousPipelineCount)
			this->Pipelines.resize(PreviousPipelineCount);
		this->TrimModuleCache();
		throw;
	}
}
GraphicsPipeline &ShaderLibrary::GetGraphicsPipeline(uint32 PipelineIndex)
{
	if (PipelineIndex >= this->Pipelines.size())
		throw std::out_of_range("Shader pipeline index is out of range");
	return *this->Pipelines[PipelineIndex].Active;
}
uint32 ShaderLibrary::CreateComputePipeline(const ComputePipelineDescriptor &Descriptor)
{
	const std::string Key = MakeComputePipelineKey(Descriptor, this->Assets);
	if (const auto Found = this->ComputePipelineCache.find(Key); Found != this->ComputePipelineCache.end())
	{
		if (Found->second >= this->ComputePipelines.size())
			throw std::logic_error("Shader compute-pipeline cache contains an invalid entry");
		return Found->second;
	}
	ComputePipelineEntry Entry{.Descriptor = Descriptor};
	try
	{
		Entry.Active = this->Build(Entry.Descriptor, Entry.ComputeGeneration, Entry.ComputeModuleKey);
	}
	catch (...)
	{
		this->TrimModuleCache();
		throw;
	}
	const usize PreviousPipelineCount = this->ComputePipelines.size();
	try
	{
		this->ComputePipelines.push_back(std::move(Entry));
		const uint32 Index = static_cast<uint32>(this->ComputePipelines.size() - 1);
		const auto [CacheIterator, Inserted] = this->ComputePipelineCache.emplace(Key, Index);
		if (!Inserted)
		{
			this->ComputePipelines.resize(PreviousPipelineCount);
			this->TrimModuleCache();
			if (CacheIterator->second >= this->ComputePipelines.size())
				throw std::logic_error("Shader compute-pipeline cache contains an invalid entry");
			return CacheIterator->second;
		}
		return Index;
	}
	catch (...)
	{
		if (this->ComputePipelines.size() > PreviousPipelineCount)
			this->ComputePipelines.resize(PreviousPipelineCount);
		this->TrimModuleCache();
		throw;
	}
}
ComputePipeline &ShaderLibrary::GetComputePipeline(uint32 PipelineIndex)
{
	if (PipelineIndex >= this->ComputePipelines.size())
		throw std::out_of_range("Compute pipeline index is out of range");
	return *this->ComputePipelines[PipelineIndex].Active;
}
void ShaderLibrary::BeginFrame()
{
	this->CollectRetiredPipelines();
	// Asset import/reload is scheduled by the owning editor session. The render
	// thread only observes published source generations and realizes replacements.
	for (PipelineEntry &Entry : this->Pipelines)
	{
		try
		{
			auto Vertex = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Entry.Descriptor.Vertex.Path);
			auto Fragment = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Entry.Descriptor.Fragment.Path);
			auto VertexSource = Vertex.Pin();
			auto FragmentSource = Fragment.Pin();
			if (VertexSource == nullptr || FragmentSource == nullptr)
				throw ShaderPipelineException(ShaderStage::Vertex, Entry.Descriptor.Vertex.Path, Entry.Descriptor.Permutation,
											  "Graphics pipeline reload source asset is unavailable");
			if (VertexSource->GetStage() != ShaderStage::Vertex || FragmentSource->GetStage() != ShaderStage::Fragment)
				throw ShaderPipelineException(ShaderStage::Vertex, Entry.Descriptor.Vertex.Path, Entry.Descriptor.Permutation,
											  "Pipeline source stage does not match its descriptor");
			if (Vertex.GetPublishedGeneration() == Entry.VertexGeneration && Fragment.GetPublishedGeneration() == Entry.FragmentGeneration)
				continue;
			uint64 VertexGeneration = 0;
			uint64 FragmentGeneration = 0;
			std::string VertexModuleKey;
			std::string FragmentModuleKey;
			auto Replacement = this->Build(Entry.Descriptor, VertexGeneration, FragmentGeneration, VertexModuleKey, FragmentModuleKey);
			const std::string PreviousVertexModuleKey = Entry.VertexModuleKey;
			const std::string PreviousFragmentModuleKey = Entry.FragmentModuleKey;
			std::unique_ptr<GraphicsPipeline> PreviousPipeline = std::move(Entry.Active);
			try
			{
				RetiredGraphicsPipeline Retired;
				Retired.VertexModuleKey = PreviousVertexModuleKey;
				Retired.FragmentModuleKey = PreviousFragmentModuleKey;
				Retired.FramesRemaining = PipelineRetirementFrameCount;
				this->RetiredGraphicsPipelines.push_back(std::move(Retired));
				this->RetiredGraphicsPipelines.back().Pipeline = std::move(PreviousPipeline);
			}
			catch (...)
			{
				Entry.Active = std::move(PreviousPipeline);
				throw;
			}
			Entry.Active = std::move(Replacement);
			Entry.VertexModuleKey = std::move(VertexModuleKey);
			Entry.FragmentModuleKey = std::move(FragmentModuleKey);
			Entry.VertexGeneration = VertexGeneration;
			Entry.FragmentGeneration = FragmentGeneration;
		}
		catch (const std::exception &Exception)
		{
			this->LastDiagnostic = Exception.what();
		}
		catch (...)
		{
			this->LastDiagnostic = "Graphics shader pipeline reload failed with a non-standard exception";
		}
	}
	for (ComputePipelineEntry &Entry : this->ComputePipelines)
	{
		try
		{
			auto Compute = this->Assets.GetAsset<ShaderSourceAsset>(resource::AssetType::ShaderSource, Entry.Descriptor.Compute.Path);
			auto ComputeSource = Compute.Pin();
			if (ComputeSource == nullptr)
				throw ShaderPipelineException(ShaderStage::Compute, Entry.Descriptor.Compute.Path, Entry.Descriptor.Permutation,
											  "Compute pipeline reload source asset is unavailable");
			if (ComputeSource->GetStage() != ShaderStage::Compute)
				throw ShaderPipelineException(ShaderStage::Compute, Entry.Descriptor.Compute.Path, Entry.Descriptor.Permutation,
											  "Compute source stage does not match its descriptor");
			if (Compute.GetPublishedGeneration() == Entry.ComputeGeneration)
				continue;
			uint64 ComputeGeneration = 0;
			std::string ComputeModuleKey;
			auto Replacement = this->Build(Entry.Descriptor, ComputeGeneration, ComputeModuleKey);
			const std::string PreviousComputeModuleKey = Entry.ComputeModuleKey;
			std::unique_ptr<ComputePipeline> PreviousPipeline = std::move(Entry.Active);
			try
			{
				RetiredComputePipeline Retired;
				Retired.ComputeModuleKey = PreviousComputeModuleKey;
				Retired.FramesRemaining = PipelineRetirementFrameCount;
				this->RetiredComputePipelines.push_back(std::move(Retired));
				this->RetiredComputePipelines.back().Pipeline = std::move(PreviousPipeline);
			}
			catch (...)
			{
				Entry.Active = std::move(PreviousPipeline);
				throw;
			}
			Entry.Active = std::move(Replacement);
			Entry.ComputeModuleKey = std::move(ComputeModuleKey);
			Entry.ComputeGeneration = ComputeGeneration;
		}
		catch (const std::exception &Exception)
		{
			this->LastDiagnostic = Exception.what();
		}
		catch (...)
		{
			this->LastDiagnostic = "Compute shader pipeline reload failed with a non-standard exception";
		}
	}
	this->TrimModuleCache();
}
void ShaderLibrary::CollectRetiredPipelines()
{
	const auto IsExpired = [](auto &Entry)
	{
		if (Entry.FramesRemaining != 0)
			--Entry.FramesRemaining;
		return Entry.FramesRemaining == 0;
	};
	std::erase_if(this->RetiredGraphicsPipelines, IsExpired);
	std::erase_if(this->RetiredComputePipelines, IsExpired);
}
void ShaderLibrary::TrimModuleCache()
{
	this->ActiveModuleKeys.clear();
	for (const PipelineEntry &Entry : this->Pipelines)
	{
		if (!Entry.VertexModuleKey.empty())
			this->ActiveModuleKeys.push_back(&Entry.VertexModuleKey);
		if (!Entry.FragmentModuleKey.empty())
			this->ActiveModuleKeys.push_back(&Entry.FragmentModuleKey);
	}
	for (const ComputePipelineEntry &Entry : this->ComputePipelines)
		if (!Entry.ComputeModuleKey.empty())
			this->ActiveModuleKeys.push_back(&Entry.ComputeModuleKey);
	for (const RetiredGraphicsPipeline &Entry : this->RetiredGraphicsPipelines)
	{
		if (!Entry.VertexModuleKey.empty())
			this->ActiveModuleKeys.push_back(&Entry.VertexModuleKey);
		if (!Entry.FragmentModuleKey.empty())
			this->ActiveModuleKeys.push_back(&Entry.FragmentModuleKey);
	}
	for (const RetiredComputePipeline &Entry : this->RetiredComputePipelines)
		if (!Entry.ComputeModuleKey.empty())
			this->ActiveModuleKeys.push_back(&Entry.ComputeModuleKey);
	for (auto Iterator = this->Modules.begin(); Iterator != this->Modules.end();)
	{
		const bool Active =
			std::ranges::any_of(this->ActiveModuleKeys, [&Iterator](const std::string *Key) { return *Key == Iterator->first; });
		if (Active)
			++Iterator;
		else
			Iterator = this->Modules.erase(Iterator);
	}
}
const std::string &ShaderLibrary::GetLastDiagnostic() const noexcept
{
	return this->LastDiagnostic;
}
} // namespace pipeline::shader
