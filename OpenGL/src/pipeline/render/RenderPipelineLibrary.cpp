#include "RenderPipelineLibrary.h"

#include "src/core/threading/TaskScheduler.h"
#include "src/pipeline/render/RenderData.h"
#include "src/pipeline/shader/ShaderSourceAsset.h"

#include <array>
#include <stdexcept>

namespace pipeline::render
{
namespace
{
[[nodiscard]] constexpr uint64 UniformInterface(const RendererBinding Binding) noexcept
{
	return pipeline::shader::UniformBlockBindingBit(static_cast<uint32>(Binding));
}

[[nodiscard]] constexpr uint64 StorageInterface(const RendererBinding Binding) noexcept
{
	return pipeline::shader::StorageBlockBindingBit(static_cast<uint32>(Binding));
}
} // namespace

void RenderPipelineLibrary::PreloadShaderSources(resource::AssetManager &Assets, core::threading::TaskScheduler &Scheduler)
{
	static constexpr std::array<string_view, 30> ShaderPaths{"shader/ShadowDepth.vert",
															 "shader/ShadowDepth.frag",
															 "shader/GBuffer.vert",
															 "shader/DepthMasked.frag",
															 "shader/GBuffer.frag",
															 "shader/TransparentOIT.frag",
															 "shader/AccuratePicking.frag",
															 "shader/EditorWireframe.frag",
															 "shader/EditorGrid.vert",
															 "shader/EditorGrid.frag",
															 "shader/GizmoOverlay.vert",
															 "shader/GizmoOverlay.frag",
															 "shader/EditorDebugLine.vert",
															 "shader/EditorDebugLine.frag",
															 "shader/Fullscreen.vert",
															 "shader/ToneMap.frag",
															 "shader/Visibility.comp",
															 "shader/VisibilityPrefixScan.comp",
															 "shader/VisibilityBlockPrefixScan.comp",
															 "shader/VisibilityFinalize.comp",
															 "shader/VisibilityScatter.comp",
															 "shader/HiZ.comp",
															 "shader/ClusteredLights.comp",
															 "shader/DeferredLighting.comp",
															 "shader/OITComposite.comp",
															 "shader/TAA.comp",
															 "shader/ViewportVisualization.comp",
															 "shader/SelectionOutline.comp",
															 "shader/AutoExposure.comp",
															 "shader/Bloom.comp"};
	Scheduler
		.Submit(
			[&Assets]()
			{
				for (const string_view Path : ShaderPaths)
				{
					const resource::AssetHandle<pipeline::shader::ShaderSourceAsset> Source =
						Assets.GetAsset<pipeline::shader::ShaderSourceAsset>(resource::AssetType::ShaderSource, Path);
					if (Source.Pin() == nullptr)
						throw std::logic_error("Shader source preload returned an empty CPU asset");
				}
			},
			core::threading::TaskPriority::Critical)
		.get();
}

RenderPipelineLibrary::RenderPipelineLibrary(pipeline::device::Device &Device, resource::AssetManager &Assets, const bool ManualSRGBEncode)
	: Shaders(Device, Assets)
{
	this->ShadowDepthPipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/ShadowDepth.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/ShadowDepth.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.DepthStencil = {.DepthTest = true, .DepthWrite = true, .DepthCompare = pipeline::shader::CompareFunction::Less},
				   .RenderTargets = {.ColorAttachmentCount = 0, .HasDepth = true, .DepthFormat = GL_DEPTH_COMPONENT32F}},
		 .RequiredVertexUniforms = pipeline::shader::UniformBit(pipeline::shader::VertexUniform::ShadowViewIndex),
		 .RequiredVertexEngineInterface = StorageInterface(RendererBinding::Instances) | StorageInterface(RendererBinding::ShadowData) |
										  StorageInterface(RendererBinding::SkinMatrices) | StorageInterface(RendererBinding::MorphDeltas) |
										  StorageInterface(RendererBinding::MorphWeights),
		 .RequiredFragmentEngineInterface = StorageInterface(RendererBinding::Materials)});
	this->DepthPrepassPipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/GBuffer.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/DepthMasked.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.DepthStencil = {.DepthTest = true, .DepthWrite = true, .DepthCompare = pipeline::shader::CompareFunction::Greater},
				   .RenderTargets = {.ColorAttachmentCount = 0, .HasDepth = true, .DepthFormat = GL_DEPTH_COMPONENT32F}},
		 .RequiredVertexEngineInterface = UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Instances) |
										  StorageInterface(RendererBinding::SkinMatrices) | StorageInterface(RendererBinding::MorphDeltas) |
										  StorageInterface(RendererBinding::MorphWeights),
		 .RequiredFragmentEngineInterface = StorageInterface(RendererBinding::Materials)});
	this->GBufferPipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/GBuffer.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/GBuffer.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.DepthStencil = {.DepthTest = true,
									.DepthWrite = false,
									.DepthCompare = pipeline::shader::CompareFunction::GreaterEqual},
				   .RenderTargets = {.ColorAttachmentCount = 6,
									 .HasDepth = true,
									 .ColorFormats = {GL_RGBA16F, GL_RGBA16F, GL_RGBA16F, GL_RG16F, GL_R32UI, GL_R8},
									 .DepthFormat = GL_DEPTH_COMPONENT32F}},
		 .RequiredFragmentUniforms = pipeline::shader::UniformBit(pipeline::shader::FragmentUniform::TrackOverdraw),
		 .RequiredVertexEngineInterface = UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Instances) |
										  StorageInterface(RendererBinding::SkinMatrices) | StorageInterface(RendererBinding::MorphDeltas) |
										  StorageInterface(RendererBinding::MorphWeights),
		 .RequiredFragmentEngineInterface = StorageInterface(RendererBinding::Materials),
		 .RequiredFragmentResources = {{.Name = "overdrawImage",
										.ResourceClass = pipeline::shader::ShaderResourceClass::Image,
										.Type = GL_UNSIGNED_INT_IMAGE_2D,
										.Binding = 7}}});
	this->TransparentOITPipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/GBuffer.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/TransparentOIT.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.DepthStencil = {.DepthTest = true, .DepthWrite = false},
				   .ColorAttachmentBlends = {{.Enabled = true,
											  .SourceColor = pipeline::shader::BlendFactor::One,
											  .DestinationColor = pipeline::shader::BlendFactor::One,
											  .SourceAlpha = pipeline::shader::BlendFactor::One,
											  .DestinationAlpha = pipeline::shader::BlendFactor::One},
											 {.Enabled = true,
											  .SourceColor = pipeline::shader::BlendFactor::Zero,
											  .DestinationColor = pipeline::shader::BlendFactor::OneMinusSourceColor,
											  .SourceAlpha = pipeline::shader::BlendFactor::Zero,
											  .DestinationAlpha = pipeline::shader::BlendFactor::OneMinusSourceAlpha}},
				   .RenderTargets = {.ColorAttachmentCount = 2,
									 .HasDepth = true,
									 .ColorFormats = {GL_RGBA16F, GL_R32F},
									 .DepthFormat = GL_DEPTH_COMPONENT32F}},
		 .RequiredFragmentUniforms = pipeline::shader::UniformBit(pipeline::shader::FragmentUniform::TrackOverdraw) |
									 pipeline::shader::UniformBit(pipeline::shader::FragmentUniform::LightCount) |
									 pipeline::shader::UniformBit(pipeline::shader::FragmentUniform::ClusterCount),
		 .RequiredVertexEngineInterface = UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Instances) |
										  StorageInterface(RendererBinding::SkinMatrices) | StorageInterface(RendererBinding::MorphDeltas) |
										  StorageInterface(RendererBinding::MorphWeights),
		 .RequiredFragmentEngineInterface =
			 UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Materials) |
			 StorageInterface(RendererBinding::Lights) | StorageInterface(RendererBinding::ClusterHeaders) |
			 StorageInterface(RendererBinding::ClusterIndices) | StorageInterface(RendererBinding::ShadowData),
		 .RequiredFragmentResources = {
			 {.Name = "directionalShadowMap",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D_ARRAY,
			  .Binding = 4},
			 {.Name = "spotShadowMap",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D_ARRAY,
			  .Binding = 5},
			 {.Name = "pointShadowMap",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_CUBE_MAP_ARRAY,
			  .Binding = 6},
			 {.Name = "opaqueHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 7},
			 {.Name = "overdrawImage",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Image,
			  .Type = GL_UNSIGNED_INT_IMAGE_2D,
			  .Binding = 7}}});
	this->AccuratePickingPipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/GBuffer.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/AccuratePicking.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.DepthStencil = {.DepthTest = true, .DepthWrite = true, .DepthCompare = pipeline::shader::CompareFunction::Greater},
				   .RenderTargets =
					   {.ColorAttachmentCount = 1, .HasDepth = true, .ColorFormats = {GL_R32UI}, .DepthFormat = GL_DEPTH_COMPONENT32F}},
		 .RequiredVertexEngineInterface = UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Instances) |
										  StorageInterface(RendererBinding::SkinMatrices) | StorageInterface(RendererBinding::MorphDeltas) |
										  StorageInterface(RendererBinding::MorphWeights),
		 .RequiredFragmentEngineInterface = StorageInterface(RendererBinding::Materials)});
	this->EditorWireframePipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/GBuffer.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/EditorWireframe.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.Rasterizer = {.CullMode = pipeline::shader::CullMode::None, .Wireframe = true},
				   .DepthStencil = {.DepthTest = true,
									.DepthWrite = false,
									.DepthCompare = pipeline::shader::CompareFunction::GreaterEqual},
				   .RenderTargets =
					   {.ColorAttachmentCount = 1, .HasDepth = true, .ColorFormats = {GL_RGBA16F}, .DepthFormat = GL_DEPTH_COMPONENT32F}},
		 .RequiredVertexEngineInterface = UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Instances) |
										  StorageInterface(RendererBinding::SkinMatrices) | StorageInterface(RendererBinding::MorphDeltas) |
										  StorageInterface(RendererBinding::MorphWeights)});
	this->EditorGridPipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/EditorGrid.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/EditorGrid.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.Rasterizer = {.CullMode = pipeline::shader::CullMode::None},
				   .DepthStencil = {.DepthTest = true,
									.DepthWrite = false,
									.DepthCompare = pipeline::shader::CompareFunction::GreaterEqual},
				   .Blend = {.Enabled = true,
							 .SourceColor = pipeline::shader::BlendFactor::SourceAlpha,
							 .DestinationColor = pipeline::shader::BlendFactor::OneMinusSourceAlpha,
							 .SourceAlpha = pipeline::shader::BlendFactor::One,
							 .DestinationAlpha = pipeline::shader::BlendFactor::OneMinusSourceAlpha},
				   .RenderTargets =
					   {.ColorAttachmentCount = 1, .HasDepth = true, .ColorFormats = {GL_RGBA16F}, .DepthFormat = GL_DEPTH_COMPONENT32F}},
		 .RequiredVertexEngineInterface = UniformInterface(RendererBinding::FrameConstants),
		 .RequiredFragmentEngineInterface = UniformInterface(RendererBinding::FrameConstants)});
	this->GizmoOverlayPipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/GizmoOverlay.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/GizmoOverlay.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.Rasterizer = {.CullMode = pipeline::shader::CullMode::None},
				   .DepthStencil = {.DepthTest = false, .DepthWrite = false},
				   .Blend = {.Enabled = true,
							 .SourceColor = pipeline::shader::BlendFactor::SourceAlpha,
							 .DestinationColor = pipeline::shader::BlendFactor::OneMinusSourceAlpha,
							 .SourceAlpha = pipeline::shader::BlendFactor::One,
							 .DestinationAlpha = pipeline::shader::BlendFactor::OneMinusSourceAlpha},
				   .RenderTargets = {.ColorAttachmentCount = 1, .HasDepth = false, .ColorFormats = {GL_RGBA16F}}},
		 .RequiredVertexUniforms = pipeline::shader::UniformBit(pipeline::shader::VertexUniform::GizmoPivot) |
								   pipeline::shader::UniformBit(pipeline::shader::VertexUniform::GizmoBasis) |
								   pipeline::shader::UniformBit(pipeline::shader::VertexUniform::GizmoScale) |
								   pipeline::shader::UniformBit(pipeline::shader::VertexUniform::GizmoOperation) |
								   pipeline::shader::UniformBit(pipeline::shader::VertexUniform::ActiveHandle),
		 .RequiredVertexEngineInterface = UniformInterface(RendererBinding::FrameConstants)});
	this->EditorDebugLinePipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/EditorDebugLine.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/EditorDebugLine.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .State = {.Rasterizer = {.CullMode = pipeline::shader::CullMode::None},
				   .DepthStencil = {.DepthTest = true,
									.DepthWrite = false,
									.DepthCompare = pipeline::shader::CompareFunction::GreaterEqual},
				   .Blend = {.Enabled = true,
							 .SourceColor = pipeline::shader::BlendFactor::SourceAlpha,
							 .DestinationColor = pipeline::shader::BlendFactor::OneMinusSourceAlpha,
							 .SourceAlpha = pipeline::shader::BlendFactor::One,
							 .DestinationAlpha = pipeline::shader::BlendFactor::OneMinusSourceAlpha},
				   .RenderTargets =
					   {.ColorAttachmentCount = 1, .HasDepth = true, .ColorFormats = {GL_RGBA16F}, .DepthFormat = GL_DEPTH_COMPONENT32F}},
		 .RequiredVertexEngineInterface =
			 UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::DebugLines)});

	pipeline::shader::ShaderPermutationKey ToneMapPermutation;
	ToneMapPermutation.Set(pipeline::shader::ShaderFeature::ManualSRGBEncode, ManualSRGBEncode);
	this->ToneMapPipeline = this->Shaders.CreateGraphicsPipeline(
		{.Vertex = {.Path = "shader/Fullscreen.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
		 .Fragment = {.Path = "shader/ToneMap.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
		 .Permutation = ToneMapPermutation,
		 .State = {.Rasterizer = {.CullMode = pipeline::shader::CullMode::None},
				   .DepthStencil = {.DepthTest = false, .DepthWrite = false},
				   .RenderTargets = {.ColorAttachmentCount = 1, .HasDepth = false, .ColorFormats = {GL_SRGB8_ALPHA8}}},
		 .RequiredFragmentResources = {
			 {.Name = "sceneTexture", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 0},
			 {.Name = "bloomTexture", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 1},
			 {.Name = "exposureTexture",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 2}}});

	this->VisibilityCullPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/Visibility.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::CandidateCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::PyramidMipCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::HistoryValid) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::ScratchCapacity),
		 .RequiredEngineInterface = UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Candidates) |
									StorageInterface(RendererBinding::VisibilityScratch),
		 .RequiredResources = {{.Name = "previousHierarchicalDepth",
								.ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
								.Type = GL_SAMPLER_2D,
								.Binding = 0}}});
	this->VisibilityPrefixScanPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/VisibilityPrefixScan.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::BatchCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::ScratchCapacity),
		 .RequiredEngineInterface = StorageInterface(RendererBinding::VisibilityScratch)});
	this->VisibilityBlockPrefixScanPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/VisibilityBlockPrefixScan.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::BlockCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::ScratchCapacity),
		 .RequiredEngineInterface = StorageInterface(RendererBinding::VisibilityScratch)});
	this->VisibilityFinalizePipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/VisibilityFinalize.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::BatchCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::ScratchCapacity),
		 .RequiredEngineInterface =
			 StorageInterface(RendererBinding::VisibilityScratch) | StorageInterface(RendererBinding::IndirectCommands)});
	this->VisibilityScatterPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/VisibilityScatter.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::CandidateCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::ScratchCapacity),
		 .RequiredEngineInterface = StorageInterface(RendererBinding::Candidates) | StorageInterface(RendererBinding::Instances) |
									StorageInterface(RendererBinding::VisibilityScratch) |
									StorageInterface(RendererBinding::IndirectCommands)});
	this->HierarchicalDepthPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/HiZ.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::SourceExtent) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::SourceMip) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::SourceScale),
		 .RequiredResources = {
			 {.Name = "sourceDepth", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 0},
			 {.Name = "destinationPyramid",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Image,
			  .Type = GL_IMAGE_2D,
			  .Binding = 0}}});
	this->ClusteredLightsPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/ClusteredLights.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::LightCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::ClusterCount),
		 .RequiredEngineInterface = UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Lights) |
									StorageInterface(RendererBinding::ClusterHeaders) | StorageInterface(RendererBinding::ClusterIndices)});
	this->DeferredLightingPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/DeferredLighting.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::LightCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::ClusterCount),
		 .RequiredEngineInterface = UniformInterface(RendererBinding::FrameConstants) | StorageInterface(RendererBinding::Lights) |
									StorageInterface(RendererBinding::ClusterHeaders) | StorageInterface(RendererBinding::ClusterIndices) |
									StorageInterface(RendererBinding::ShadowData),
		 .RequiredResources = {
			 {.Name = "gbufferBaseColor",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 0},
			 {.Name = "gbufferNormalRoughness",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 1},
			 {.Name = "gbufferMaterial",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 2},
			 {.Name = "depthTexture", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 3},
			 {.Name = "directionalShadowMap",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D_ARRAY,
			  .Binding = 4},
			 {.Name = "spotShadowMap",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D_ARRAY,
			  .Binding = 5},
			 {.Name = "pointShadowMap",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_CUBE_MAP_ARRAY,
			  .Binding = 6},
			 {.Name = "gbufferOcclusion",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 7},
			 {.Name = "outputLighting",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Image,
			  .Type = GL_IMAGE_2D,
			  .Binding = 0}}});
	this->OITCompositionPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/OITComposite.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredResources = {
			 {.Name = "opaqueHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 0},
			 {.Name = "accumulationTexture",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 1},
			 {.Name = "revealageTexture",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 2},
			 {.Name = "outputHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Image, .Type = GL_IMAGE_2D, .Binding = 0}}});
	this->TemporalAAPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/TAA.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::HistoryValid),
		 .RequiredResources = {
			 {.Name = "currentHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 0},
			 {.Name = "historyHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 1},
			 {.Name = "velocityTexture",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 2},
			 {.Name = "currentObjectIDs",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_UNSIGNED_INT_SAMPLER_2D,
			  .Binding = 3},
			 {.Name = "historyObjectIDs",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_UNSIGNED_INT_SAMPLER_2D,
			  .Binding = 4},
			 {.Name = "nextHistory", .ResourceClass = pipeline::shader::ShaderResourceClass::Image, .Type = GL_IMAGE_2D, .Binding = 0},
			 {.Name = "resolvedHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Image, .Type = GL_IMAGE_2D, .Binding = 1},
			 {.Name = "nextObjectIDHistory",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Image,
			  .Type = GL_UNSIGNED_INT_IMAGE_2D,
			  .Binding = 2}}});
	this->ViewportVisualizationPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/ViewportVisualization.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::ViewMode),
		 .RequiredEngineInterface = UniformInterface(RendererBinding::FrameConstants),
		 .RequiredResources = {
			 {.Name = "litHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 0},
			 {.Name = "baseColor", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 1},
			 {.Name = "normalRoughness",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_SAMPLER_2D,
			  .Binding = 2},
			 {.Name = "materialData", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 3},
			 {.Name = "sceneDepth", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 4},
			 {.Name = "objectIDs",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_UNSIGNED_INT_SAMPLER_2D,
			  .Binding = 5},
			 {.Name = "overdrawCounts",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_UNSIGNED_INT_SAMPLER_2D,
			  .Binding = 6},
			 {.Name = "visualizedHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Image, .Type = GL_IMAGE_2D, .Binding = 0}}});
	this->SelectionOutlinePipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/SelectionOutline.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::SelectionWordCount) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::OutlineRadius) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::OutlineColor),
		 .RequiredEngineInterface = StorageInterface(RendererBinding::SelectionMask),
		 .RequiredResources = {
			 {.Name = "sourceHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 0},
			 {.Name = "objectIDs",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
			  .Type = GL_UNSIGNED_INT_SAMPLER_2D,
			  .Binding = 1},
			 {.Name = "outlinedHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Image, .Type = GL_IMAGE_2D, .Binding = 0}}});
	this->AutoExposurePipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/AutoExposure.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::HistoryValid) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::DeltaSeconds),
		 .RequiredResources = {
			 {.Name = "sourceHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 0},
			 {.Name = "exposureOutput",
			  .ResourceClass = pipeline::shader::ShaderResourceClass::Image,
			  .Type = GL_IMAGE_2D,
			  .Binding = 0}}});
	this->BloomPipeline = this->Shaders.CreateComputePipeline(
		{.Compute = {.Path = "shader/Bloom.comp", .Stage = pipeline::shader::ShaderStage::Compute},
		 .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::SourceMip) |
							 pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::Operation),
		 .RequiredResources = {
			 {.Name = "sourceHDR", .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler, .Type = GL_SAMPLER_2D, .Binding = 0},
			 {.Name = "bloomOutput", .ResourceClass = pipeline::shader::ShaderResourceClass::Image, .Type = GL_IMAGE_2D, .Binding = 0}}});
}

void RenderPipelineLibrary::BeginFrame()
{
	this->Shaders.BeginFrame();
}

RenderPassPipelineSet RenderPipelineLibrary::GetPipelineSet()
{
	return {.ShadowDepth = this->Shaders.GetGraphicsPipeline(this->ShadowDepthPipeline),
			.DepthPrepass = this->Shaders.GetGraphicsPipeline(this->DepthPrepassPipeline),
			.GBuffer = this->Shaders.GetGraphicsPipeline(this->GBufferPipeline),
			.TransparentOIT = this->Shaders.GetGraphicsPipeline(this->TransparentOITPipeline),
			.AccuratePicking = this->Shaders.GetGraphicsPipeline(this->AccuratePickingPipeline),
			.EditorWireframe = this->Shaders.GetGraphicsPipeline(this->EditorWireframePipeline),
			.EditorGrid = this->Shaders.GetGraphicsPipeline(this->EditorGridPipeline),
			.EditorDebugLine = this->Shaders.GetGraphicsPipeline(this->EditorDebugLinePipeline),
			.GizmoOverlay = this->Shaders.GetGraphicsPipeline(this->GizmoOverlayPipeline),
			.ToneMap = this->Shaders.GetGraphicsPipeline(this->ToneMapPipeline),
			.VisibilityCull = this->Shaders.GetComputePipeline(this->VisibilityCullPipeline),
			.VisibilityPrefixScan = this->Shaders.GetComputePipeline(this->VisibilityPrefixScanPipeline),
			.VisibilityBlockPrefixScan = this->Shaders.GetComputePipeline(this->VisibilityBlockPrefixScanPipeline),
			.VisibilityFinalize = this->Shaders.GetComputePipeline(this->VisibilityFinalizePipeline),
			.VisibilityScatter = this->Shaders.GetComputePipeline(this->VisibilityScatterPipeline),
			.HierarchicalDepth = this->Shaders.GetComputePipeline(this->HierarchicalDepthPipeline),
			.ClusteredLights = this->Shaders.GetComputePipeline(this->ClusteredLightsPipeline),
			.DeferredLighting = this->Shaders.GetComputePipeline(this->DeferredLightingPipeline),
			.OITComposition = this->Shaders.GetComputePipeline(this->OITCompositionPipeline),
			.TemporalAA = this->Shaders.GetComputePipeline(this->TemporalAAPipeline),
			.ViewportVisualization = this->Shaders.GetComputePipeline(this->ViewportVisualizationPipeline),
			.SelectionOutline = this->Shaders.GetComputePipeline(this->SelectionOutlinePipeline),
			.AutoExposure = this->Shaders.GetComputePipeline(this->AutoExposurePipeline),
			.Bloom = this->Shaders.GetComputePipeline(this->BloomPipeline)};
}

const string &RenderPipelineLibrary::GetLastDiagnostic() const noexcept
{
	return this->Shaders.GetLastDiagnostic();
}
} // namespace pipeline::render
