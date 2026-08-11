#include "Renderer.h"

#include "src/core/window/Window.h"
#include "src/pipeline/device/Device.h"
#include "src/pipeline/render/SceneExtractor.h"
#include "src/pipeline/render/ViewportPicker.h"
#include "src/resource/asset/AssetManager.h"
#include "src/scene/Camera.h"
#include "src/scene/Scene.h"

#include <bit>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pipeline::render
{
namespace
{
constexpr uint32 MaximumRenderItems = 65'536;
constexpr uint32 MaximumSkinMatrices = 262'144;
constexpr uint32 MaximumMorphWeights = 262'144;
constexpr uint32 MaximumDebugLines = 262'144;
constexpr uint64 HashSeed = 1469598103934665603ULL;
constexpr uint64 HashPrime = 1099511628211ULL;
[[nodiscard]] uint64 HashValue(uint64 Hash, uint32 Value) noexcept
{
	return (Hash ^ Value) * HashPrime;
}
[[nodiscard]] uint64 HashValue(uint64 Hash, const uint64 Value) noexcept
{
	Hash = HashValue(Hash, static_cast<uint32>(Value));
	return HashValue(Hash, static_cast<uint32>(Value >> 32U));
}
[[nodiscard]] uint64 HashBytes(uint64 Hash, const void *Data, const usize Size) noexcept
{
	const auto *Bytes = static_cast<const uint8 *>(Data);
	for (usize Index = 0; Index < Size; ++Index)
	{
		Hash ^= Bytes[Index];
		Hash *= HashPrime;
	}
	return Hash;
}
[[nodiscard]] uint64 HashMatrix(uint64 Hash, const glm::mat4 &Matrix) noexcept
{
	const float32 *const Values = glm::value_ptr(Matrix);
	for (uint32 Index = 0; Index < 16; ++Index)
		Hash = HashValue(Hash, std::bit_cast<uint32>(Values[Index]));
	return Hash;
}

[[nodiscard]] uint64 CalculateShadowCasterSignature(const SceneCollection &Collection) noexcept
{
	uint64 Hash = HashSeed;
	for (const pipeline::render::RenderItem &Item : Collection.GetRenderItems())
	{
		if (!Item.CastsShadows)
			continue;
		Hash = HashValue(Hash, Item.ObjectID);
		Hash = HashValue(Hash, Item.Revision);
		Hash = HashValue(Hash, Item.VertexArray);
		Hash = HashValue(Hash, Item.FirstIndex);
		Hash = HashValue(Hash, Item.IndexCount);
		Hash = HashValue(Hash, static_cast<uint32>(Item.BaseVertex));
		Hash = HashValue(Hash, Item.MorphDeltaBuffer);
		Hash = HashValue(Hash, Item.MorphVertexCount);
		Hash = HashValue(Hash, Item.SkinPaletteOffset);
		Hash = HashValue(Hash, Item.MorphWeightOffset);
		Hash = HashValue(Hash, Item.MorphWeightCount);
		Hash = HashValue(Hash, Item.Skinned ? 1U : 0U);
		Hash = HashValue(Hash, Item.Masked ? 1U : 0U);
		Hash = HashValue(Hash, Item.TwoSided ? 1U : 0U);
		Hash = HashMatrix(Hash, Item.Transform);
		Hash = HashBytes(Hash, &Item.WorldBounds, sizeof(Item.WorldBounds));
		Hash = HashBytes(Hash, &Item.Material, sizeof(Item.Material));
	}
	const auto &SkinMatrices = Collection.GetSkinningMatrices();
	Hash = HashBytes(Hash, SkinMatrices.data(), SkinMatrices.size() * sizeof(pipeline::render::GPUSkinMatrixRecord));
	const auto &MorphWeights = Collection.GetMorphWeights();
	Hash = HashBytes(Hash, MorphWeights.data(), MorphWeights.size() * sizeof(pipeline::render::GPUMorphWeightRecord));
	return Hash;
}
[[nodiscard]] GLenum ToOpenGlIndexFormat(const pipeline::render::RenderIndexFormat Format) noexcept
{
	return Format == pipeline::render::RenderIndexFormat::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
}
[[nodiscard]] uint32 IndexElementSize(const pipeline::render::RenderIndexFormat Format) noexcept
{
	return Format == pipeline::render::RenderIndexFormat::UInt16 ? sizeof(uint16) : sizeof(uint32);
}

void CopyToMappedBuffer(const pipeline::frame::FrameBufferSlice &Destination, const void *Source, const uint64 SizeInBytes,
						std::string_view ResourceName)
{
	if (SizeInBytes == 0)
		return;
	if (Source == nullptr)
		throw std::invalid_argument(std::string(ResourceName) + " upload source is null");
	if (Destination.MappedMemory == nullptr || Destination.Buffer == 0)
		throw std::runtime_error(std::string(ResourceName) + " frame buffer is unavailable");
	if (SizeInBytes > Destination.CapacityInBytes)
		throw std::overflow_error(std::string(ResourceName) + " frame buffer capacity exceeded");
	std::memcpy(Destination.MappedMemory, Source, static_cast<usize>(SizeInBytes));
}

template <typename Callback> class ScopeExit final
{
  public:
	explicit ScopeExit(Callback &&Function) noexcept(std::is_nothrow_move_constructible_v<Callback>) : Function(std::move(Function))
	{
	}
	~ScopeExit() noexcept
	{
		if (this->Active)
			this->Function();
	}
	ScopeExit(const ScopeExit &) = delete;
	ScopeExit &operator=(const ScopeExit &) = delete;
	void Release() noexcept
	{
		this->Active = false;
	}

  private:
	Callback Function;
	bool Active = true;
};

template <typename Callback> [[nodiscard]] auto MakeScopeExit(Callback &&Function)
{
	return ScopeExit<std::decay_t<Callback>>(std::forward<Callback>(Function));
}
} // namespace

Renderer::Renderer(pipeline::device::Device &Device, const bool HeadlessPresentationValidation,
				   std::filesystem::path HeadlessPresentationCapturePath)
	: Device(Device), OwnerThread(std::this_thread::get_id()), HeadlessPresentationValidation(HeadlessPresentationValidation),
	  HeadlessPresentationCapturePath(std::move(HeadlessPresentationCapturePath)),
	  SceneCollection({.RenderItems = MaximumRenderItems,
					   .SkinningMatrices = MaximumSkinMatrices,
					   .MorphWeights = MaximumMorphWeights,
					   .PickObjects = MaximumRenderItems,
					   .DirectionalLights = pipeline::render::MaximumLightCount,
					   .PointLights = pipeline::render::MaximumLightCount,
					   .SpotLights = pipeline::render::MaximumLightCount}),
	  MeshGPUCache(Device), ScenePreparation(MaximumRenderItems), MainPreparationWorkspace(MaximumRenderItems),
	  ShadowPreparationWorkspace(MaximumRenderItems), LightBufferManager(pipeline::render::MaximumLightCount)
{
	this->Prepared.Reserve(MaximumRenderItems);
	this->ShadowPrepared.Reserve(MaximumRenderItems);
	this->ShadowVisibleInstances.reserve(MaximumRenderItems);
	this->ShadowDrawRanges.reserve(MaximumRenderItems);
	this->ShadowRecords.resize(pipeline::render::MaximumShadowRecordCount);
	this->PendingPickPixels.reserve(pipeline::render::ViewportPicker::ReadbackSlotCount);
	this->PendingDebugLines.reserve(MaximumDebugLines);
	this->PendingDebugBounds.reserve(MaximumRenderItems);
	this->ShadowLayerCache.reserve(pipeline::render::MaximumShadowRecordCount);
	(void)this->Device->RequireCurrentContext();
	auto ResourceCleanup = MakeScopeExit(
		[this]() noexcept
		{
			if (this->ShadowFramebuffer != 0)
				glDeleteFramebuffers(1, &this->ShadowFramebuffer);
			if (this->PresentationFramebuffer != 0)
				glDeleteFramebuffers(1, &this->PresentationFramebuffer);
			if (this->FullscreenVertexArray != 0)
				glDeleteVertexArrays(1, &this->FullscreenVertexArray);
			this->ShadowFramebuffer = 0;
			this->PresentationFramebuffer = 0;
			this->FullscreenVertexArray = 0;
		});
	glCreateFramebuffers(1, &ShadowFramebuffer);
	glCreateFramebuffers(1, &PresentationFramebuffer);
	glCreateVertexArrays(1, &this->FullscreenVertexArray);
	if (ShadowFramebuffer == 0 || PresentationFramebuffer == 0 || this->FullscreenVertexArray == 0)
		throw device::DeviceError("OpenGL could not allocate the renderer framebuffer or fullscreen vertex array");
	constexpr string_view FullscreenVertexArrayLabel = "Renderer-FullscreenVertexArray";
	glObjectLabel(GL_VERTEX_ARRAY, this->FullscreenVertexArray, static_cast<GLsizei>(FullscreenVertexArrayLabel.size()),
				  FullscreenVertexArrayLabel.data());
	this->FrameResources = std::make_unique<pipeline::frame::FrameResourceRing>(
		Device, pipeline::frame::FrameResourceCapacitySpecification{
					.FrameConstants = sizeof(pipeline::render::GPUFrameConstants),
					.Materials = sizeof(pipeline::render::GPUMaterialRecord) * MaximumRenderItems,
					.ShadowData = sizeof(pipeline::render::GPUShadowRecord) * pipeline::render::MaximumShadowRecordCount,
					.Lights = sizeof(pipeline::render::GPULightRecord) * pipeline::render::MaximumLightCount,
					.CandidateInstances = sizeof(pipeline::render::PreparedInstance) * MaximumRenderItems,
					.ShadowInstances = sizeof(pipeline::render::PreparedInstance) * MaximumRenderItems,
					.VisibleInstances = sizeof(pipeline::render::PreparedInstance) * MaximumRenderItems,
					.IndirectCommands = sizeof(pipeline::render::RenderCommand) * MaximumRenderItems,
					.BatchMetadata = sizeof(pipeline::render::RenderBatch) * MaximumRenderItems,
					.VisibilityScratch = sizeof(uint32) * (MaximumRenderItems * 4U + 512U),
					.SkinMatrices = sizeof(pipeline::render::GPUSkinMatrixRecord) * MaximumSkinMatrices,
					.MorphWeights = sizeof(pipeline::render::GPUMorphWeightRecord) * MaximumMorphWeights,
					.SelectionMask = sizeof(uint32) * pipeline::render::MaximumSelectionMaskWordCount,
					.DebugLines = sizeof(pipeline::render::GPUDebugLineRecord) * MaximumDebugLines});
	this->InitializeHybridPassCallbacks();
	this->Device->CheckErrors("Renderer creation");
	ResourceCleanup.Release();
}

void Renderer::RequireOwnerThread() const
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw std::logic_error("Renderer access must remain on its owning render thread");
	(void)this->Device->RequireCurrentContext();
}

void Renderer::InitializeHybridPassCallbacks()
{
	const auto Bind = [this](const pipeline::graph::HybridDeferredPassID Pass)
	{ return pipeline::graph::HybridDeferredPassCallback{.Executor = &Renderer::DispatchHybridPass, .UserData = this, .Pass = Pass}; };
	this->HybridPassCallbacks.DirectionalShadows = Bind(pipeline::graph::HybridDeferredPassID::DirectionalShadows);
	this->HybridPassCallbacks.SpotShadows = Bind(pipeline::graph::HybridDeferredPassID::SpotShadows);
	this->HybridPassCallbacks.PointShadows = Bind(pipeline::graph::HybridDeferredPassID::PointShadows);
	this->HybridPassCallbacks.MainVisibility = Bind(pipeline::graph::HybridDeferredPassID::MainVisibility);
	this->HybridPassCallbacks.AccuratePicking = Bind(pipeline::graph::HybridDeferredPassID::AccuratePicking);
	this->HybridPassCallbacks.DepthPrepass = Bind(pipeline::graph::HybridDeferredPassID::DepthPrepass);
	this->HybridPassCallbacks.HierarchicalDepth = Bind(pipeline::graph::HybridDeferredPassID::HierarchicalDepth);
	this->HybridPassCallbacks.GBuffer = Bind(pipeline::graph::HybridDeferredPassID::GBuffer);
	this->HybridPassCallbacks.ClusteredLights = Bind(pipeline::graph::HybridDeferredPassID::ClusteredLights);
	this->HybridPassCallbacks.DeferredLighting = Bind(pipeline::graph::HybridDeferredPassID::DeferredLighting);
	this->HybridPassCallbacks.WeightedOIT = Bind(pipeline::graph::HybridDeferredPassID::WeightedOIT);
	this->HybridPassCallbacks.OITComposition = Bind(pipeline::graph::HybridDeferredPassID::OITComposition);
	this->HybridPassCallbacks.TemporalAA = Bind(pipeline::graph::HybridDeferredPassID::TemporalAA);
	this->HybridPassCallbacks.ViewportVisualization = Bind(pipeline::graph::HybridDeferredPassID::ViewportVisualization);
	this->HybridPassCallbacks.SelectionOutline = Bind(pipeline::graph::HybridDeferredPassID::SelectionOutline);
	this->HybridPassCallbacks.WireframeOverlay = Bind(pipeline::graph::HybridDeferredPassID::WireframeOverlay);
	this->HybridPassCallbacks.EditorGrid = Bind(pipeline::graph::HybridDeferredPassID::EditorGrid);
	this->HybridPassCallbacks.DebugOverlay = Bind(pipeline::graph::HybridDeferredPassID::DebugOverlay);
	this->HybridPassCallbacks.GizmoOverlay = Bind(pipeline::graph::HybridDeferredPassID::GizmoOverlay);
	this->HybridPassCallbacks.ExposureAndBloom = Bind(pipeline::graph::HybridDeferredPassID::ExposureAndBloom);
	this->HybridPassCallbacks.ToneMapAndPresent = Bind(pipeline::graph::HybridDeferredPassID::ToneMapAndPresent);
}

void Renderer::DispatchHybridPass(void *UserData, const pipeline::graph::HybridDeferredPassID Pass,
								  pipeline::graph::RenderGraphContext &Context,
								  const pipeline::graph::HybridDeferredFrameResources &Resources)
{
	if (UserData == nullptr)
		throw std::logic_error("Hybrid deferred pass dispatch has no renderer owner");
	static_cast<Renderer *>(UserData)->ExecuteHybridPass(Pass, Context, Resources);
}

void Renderer::ExecuteHybridDispatch(const pipeline::shader::ComputePipeline &Pipeline, pipeline::graph::RenderGraphContext &Context,
									 const pipeline::graph::TextureHandle Output)
{
	const pipeline::graph::Extent2D Size = Context.GetExtent(Output);
	Pipeline.Bind();
	glDispatchCompute((Size.Width + 7U) / 8U, (Size.Height + 7U) / 8U, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT |
					GL_TEXTURE_FETCH_BARRIER_BIT);
}

void Renderer::ExecuteHybridDrawBatches(pipeline::graph::RenderGraphContext &Context, const pipeline::shader::GraphicsPipeline &Pipeline,
										const pipeline::render::RenderPassClass RequiredPass)
{
	if (this->ActiveHybridPass.Frame == nullptr || this->ActiveHybridPass.Prepared == nullptr)
		throw std::logic_error("Hybrid deferred draw execution has no active frame state");
	pipeline::frame::FrameResources &Frame = *this->ActiveHybridPass.Frame;
	const pipeline::render::RenderPreparationResult &Prepared = *this->ActiveHybridPass.Prepared;
	Context.ValidateGraphicsPipelineTargets(Pipeline);
	Pipeline.Bind();
	glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::FrameConstants),
					 Frame.FrameConstants.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Instances),
					 Frame.VisibleInstances.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Materials), Frame.Materials.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::SkinMatrices),
					 Frame.SkinMatrices.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::MorphWeights),
					 Frame.MorphWeights.Buffer);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, Frame.IndirectCommands.Buffer);
	bool TwoSidedState = false;
	bool TwoSidedStateValid = false;
	for (uint32 BatchIndex = 0; BatchIndex < Prepared.Batches.size();)
	{
		const pipeline::render::RenderBatch &FirstBatch = Prepared.Batches[BatchIndex];
		if (FirstBatch.PassClass != RequiredPass)
		{
			++BatchIndex;
			continue;
		}
		if (FirstBatch.VertexDescriptor == nullptr)
			throw std::logic_error("RenderBatch is missing its VertexDescriptor");
		const uint32 GroupStart = BatchIndex;
		uint32 GroupCount = 0;
		while (BatchIndex < Prepared.Batches.size())
		{
			const pipeline::render::RenderBatch &Batch = Prepared.Batches[BatchIndex];
			if (Batch.PassClass != RequiredPass || Batch.VertexArray != FirstBatch.VertexArray ||
				Batch.VertexDescriptor != FirstBatch.VertexDescriptor || Batch.IndexFormat != FirstBatch.IndexFormat ||
				Batch.MorphDeltaBuffer != FirstBatch.MorphDeltaBuffer || Batch.TwoSided != FirstBatch.TwoSided)
				break;
			if (Batch.VertexDescriptor == nullptr)
				throw std::logic_error("RenderBatch is missing its VertexDescriptor");
			Pipeline.ValidateVertexDescriptor(*Batch.VertexDescriptor);
			++BatchIndex;
			++GroupCount;
		}
		if (GroupCount == 0)
			throw std::logic_error("Render batch grouping produced an empty compatible run");
		if (!TwoSidedStateValid || FirstBatch.TwoSided != TwoSidedState)
		{
			if (FirstBatch.TwoSided)
			{
				glDisable(GL_CULL_FACE);
				this->Device->InvalidateGraphicsPipelineStateCache();
			}
			else if (Pipeline.GetDescriptor().State.Rasterizer.CullMode != pipeline::shader::CullMode::None)
			{
				glEnable(GL_CULL_FACE);
				this->Device->InvalidateGraphicsPipelineStateCache();
			}
			TwoSidedState = FirstBatch.TwoSided;
			TwoSidedStateValid = true;
		}
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::MorphDeltas),
						 FirstBatch.MorphDeltaBuffer);
		glBindVertexArray(FirstBatch.VertexArray);
		if (GroupCount > static_cast<uint32>(std::numeric_limits<GLsizei>::max()))
			throw std::overflow_error("Render batch group exceeds OpenGL multi-draw count limits");
		const uintptr_t Offset = static_cast<uintptr_t>(GroupStart) * sizeof(pipeline::render::RenderCommand);
		glMultiDrawElementsIndirect(Pipeline.GetGLTopology(), ToOpenGlIndexFormat(FirstBatch.IndexFormat),
									reinterpret_cast<const void *>(Offset), static_cast<GLsizei>(GroupCount),
									sizeof(pipeline::render::RenderCommand));
		++this->DrawCount;
	}
}

void Renderer::ExecuteHybridShadowLayers(const GLuint Texture, const uint32 FirstLayer, const uint32 LayerCount, const uint32 FirstRecord,
										 const pipeline::graph::Extent2D ShadowExtent)
{
	if (LayerCount == 0)
		return;
	if (this->ActiveHybridPass.Pipelines == nullptr || this->ActiveHybridPass.Frame == nullptr ||
		this->ActiveHybridPass.ShadowPrepared == nullptr)
		throw std::logic_error("Hybrid deferred shadow execution has no active frame state");
	const pipeline::render::RenderPassPipelineSet &Pipelines = *this->ActiveHybridPass.Pipelines;
	pipeline::frame::FrameResources &Frame = *this->ActiveHybridPass.Frame;
	const pipeline::render::RenderPreparationResult &ShadowPrepared = *this->ActiveHybridPass.ShadowPrepared;
	Pipelines.ShadowDepth.Bind();
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Instances),
					 Frame.ShadowInstances.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Materials), Frame.Materials.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ShadowData), Frame.ShadowData.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::SkinMatrices),
					 Frame.SkinMatrices.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::MorphWeights),
					 Frame.MorphWeights.Buffer);
	glNamedFramebufferDrawBuffer(this->ShadowFramebuffer, GL_NONE);
	glNamedFramebufferReadBuffer(this->ShadowFramebuffer, GL_NONE);
	bool TwoSidedState = false;
	for (uint32 Layer = 0; Layer < LayerCount; ++Layer)
	{
		const pipeline::render::GPUShadowRecord &ShadowView = this->ShadowRecords.at(FirstRecord + Layer);
		uint64 ShadowSignature =
			HashMatrix(HashValue(this->ActiveHybridPass.ShadowCasterSignature, static_cast<uint32>(Texture)), ShadowView.ViewProjection);
		ShadowSignature = HashValue(ShadowSignature, FirstLayer + Layer);
		const uint32 CacheKey = FirstRecord + Layer;
		ShadowLayerCacheEntry &CachedLayer = this->ShadowLayerCache[CacheKey];
		if (CachedLayer.Valid && CachedLayer.Texture == Texture && CachedLayer.Layer == FirstLayer + Layer &&
			CachedLayer.Signature == ShadowSignature)
			continue;
		this->ShadowVisibleInstances.clear();
		this->ShadowDrawRanges.clear();
		for (const pipeline::render::RenderBatch &Batch : ShadowPrepared.Batches)
		{
			if (Batch.PassClass == pipeline::render::RenderPassClass::Transparency)
				continue;
			if (Batch.FirstCandidate > ShadowPrepared.CandidateInstances.size() ||
				Batch.CandidateCount > ShadowPrepared.CandidateInstances.size() - Batch.FirstCandidate)
				throw std::logic_error("Shadow batch candidate range is outside the prepared instance buffer");
			const uint32 FirstVisible = static_cast<uint32>(this->ShadowVisibleInstances.size());
			for (uint32 CandidateOffset = 0; CandidateOffset < Batch.CandidateCount; ++CandidateOffset)
			{
				const pipeline::render::PreparedInstance &Instance =
					ShadowPrepared.CandidateInstances[Batch.FirstCandidate + CandidateOffset];
				if (pipeline::render::ScenePreparation::IntersectsFrustum(Instance.WorldBounds, ShadowView.ViewProjection))
				{
					if (this->ShadowVisibleInstances.size() >= this->ShadowVisibleInstances.capacity())
						throw std::overflow_error("Shadow-view visibility exceeded its CPU capacity");
					this->ShadowVisibleInstances.push_back(Instance);
				}
			}
			const uint32 VisibleCount = static_cast<uint32>(this->ShadowVisibleInstances.size()) - FirstVisible;
			if (VisibleCount != 0)
			{
				if (this->ShadowDrawRanges.size() >= this->ShadowDrawRanges.capacity())
					throw std::overflow_error("Shadow draw-range capacity exceeded");
				this->ShadowDrawRanges.push_back({.Batch = &Batch, .FirstInstance = FirstVisible, .InstanceCount = VisibleCount});
			}
		}
		if (this->ShadowVisibleInstances.size() > Frame.ShadowInstances.CapacityInBytes / sizeof(pipeline::render::PreparedInstance))
			throw std::overflow_error("Shadow-view visibility exceeded the frame shadow-instance capacity");
		CopyToMappedBuffer(Frame.ShadowInstances, this->ShadowVisibleInstances.data(),
						   this->ShadowVisibleInstances.size() * sizeof(pipeline::render::PreparedInstance), "shadow-visible instances");
		glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
		glNamedFramebufferTextureLayer(this->ShadowFramebuffer, GL_DEPTH_ATTACHMENT, Texture, 0, static_cast<GLint>(FirstLayer + Layer));
		if (!CachedLayer.FramebufferValidated || CachedLayer.Texture != Texture || CachedLayer.Layer != FirstLayer + Layer ||
			CachedLayer.Extent.Width != ShadowExtent.Width || CachedLayer.Extent.Height != ShadowExtent.Height)
		{
			if (glCheckNamedFramebufferStatus(this->ShadowFramebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				throw std::runtime_error("Shadow framebuffer is incomplete");
			CachedLayer.FramebufferValidated = true;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, this->ShadowFramebuffer);
		glViewport(0, 0, static_cast<GLsizei>(ShadowExtent.Width), static_cast<GLsizei>(ShadowExtent.Height));
		glClearDepth(1.0);
		glClear(GL_DEPTH_BUFFER_BIT);
		Pipelines.ShadowDepth.SetVertexUniformUInt(pipeline::shader::VertexUniform::ShadowViewIndex, FirstRecord + Layer);
		for (const ShadowDrawRange &Range : this->ShadowDrawRanges)
		{
			const pipeline::render::RenderBatch &Batch = *Range.Batch;
			if (Batch.VertexDescriptor == nullptr)
				throw std::logic_error("RenderBatch is missing its VertexDescriptor");
			Pipelines.ShadowDepth.ValidateVertexDescriptor(*Batch.VertexDescriptor);
			if (Batch.TwoSided != TwoSidedState)
			{
				if (Batch.TwoSided)
					glDisable(GL_CULL_FACE);
				else if (Pipelines.ShadowDepth.GetDescriptor().State.Rasterizer.CullMode != pipeline::shader::CullMode::None)
					glEnable(GL_CULL_FACE);
				this->Device->InvalidateGraphicsPipelineStateCache();
				TwoSidedState = Batch.TwoSided;
			}
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::MorphDeltas),
							 Batch.MorphDeltaBuffer);
			glBindVertexArray(Batch.VertexArray);
			glDrawElementsInstancedBaseVertexBaseInstance(
				Pipelines.ShadowDepth.GetGLTopology(), static_cast<GLsizei>(Batch.IndexCount), ToOpenGlIndexFormat(Batch.IndexFormat),
				reinterpret_cast<const void *>(static_cast<uintptr_t>(Batch.FirstIndex) * IndexElementSize(Batch.IndexFormat)),
				static_cast<GLsizei>(Range.InstanceCount), Batch.BaseVertex, Range.FirstInstance);
		}
		CachedLayer.Texture = Texture;
		CachedLayer.Layer = FirstLayer + Layer;
		CachedLayer.Extent = ShadowExtent;
		CachedLayer.Signature = ShadowSignature;
		CachedLayer.Valid = true;
	}
}

void Renderer::ExecuteHybridPass(const pipeline::graph::HybridDeferredPassID Pass, pipeline::graph::RenderGraphContext &Context,
								 const pipeline::graph::HybridDeferredFrameResources &Resources)
{
	const HybridPassExecutionState &Active = this->ActiveHybridPass;
	if (Active.Pipelines == nullptr || Active.Frame == nullptr || Active.Prepared == nullptr || Active.ShadowPrepared == nullptr ||
		Active.View == nullptr)
		throw std::logic_error("Hybrid deferred pass execution has incomplete frame state");
	const pipeline::render::RenderPassPipelineSet &Pipelines = *Active.Pipelines;
	pipeline::frame::FrameResources &Frame = *Active.Frame;
	const pipeline::render::RenderPreparationResult &Prepared = *Active.Prepared;
	ViewState &View = *Active.View;
	const pipeline::graph::Extent2D Extent = Active.Extent;

	switch (Pass)
	{
	case pipeline::graph::HybridDeferredPassID::DirectionalShadows:
		Context.ValidateGraphicsPipelineTargets(Pipelines.ShadowDepth);
		this->ExecuteHybridShadowLayers(Context.GetTexture(Resources.DirectionalShadowAtlas), 0, Active.DirectionalCascadeCount, 0,
										Context.GetExtent(Resources.DirectionalShadowAtlas));
		return;
	case pipeline::graph::HybridDeferredPassID::SpotShadows:
		Context.ValidateGraphicsPipelineTargets(Pipelines.ShadowDepth);
		this->ExecuteHybridShadowLayers(Context.GetTexture(Resources.SpotShadowAtlas), 0, Active.SpotShadowCount,
										pipeline::render::DirectionalShadowCascadeCount, Context.GetExtent(Resources.SpotShadowAtlas));
		return;
	case pipeline::graph::HybridDeferredPassID::PointShadows:
		Context.ValidateGraphicsPipelineTargets(Pipelines.ShadowDepth);
		this->ExecuteHybridShadowLayers(Context.GetTexture(Resources.PointShadowArray), 0, Active.PointShadowFaceCount,
										pipeline::render::DirectionalShadowCascadeCount + pipeline::render::MaximumSpotShadowCount,
										Context.GetExtent(Resources.PointShadowArray));
		return;
	case pipeline::graph::HybridDeferredPassID::MainVisibility:
	{
		const uint32 Zero = 0;
		const uint32 CandidateCount = static_cast<uint32>(Prepared.CandidateInstances.size());
		const uint32 BatchCount = static_cast<uint32>(Prepared.Batches.size());
		const uint32 PyramidMipCount = static_cast<uint32>(std::bit_width(
			std::max(Context.GetExtent(Resources.HierarchicalDepthRead).Width, Context.GetExtent(Resources.HierarchicalDepthRead).Height)));
		const bool HistoryMatchesExtent =
			View.HierarchicalDepthHistoryValid &&
			View.HierarchicalDepthHistoryExtent.Width == Context.GetExtent(Resources.HierarchicalDepthRead).Width &&
			View.HierarchicalDepthHistoryExtent.Height == Context.GetExtent(Resources.HierarchicalDepthRead).Height;
		glClearNamedBufferData(Frame.VisibilityScratch.Buffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &Zero);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Candidates),
						 Frame.CandidateInstances.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Instances),
						 Frame.VisibleInstances.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::VisibilityScratch),
						 Frame.VisibilityScratch.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::IndirectCommands),
						 Frame.IndirectCommands.Buffer);
		const GLuint PreviousHierarchicalDepth = Context.GetTexture(Resources.HierarchicalDepthRead);
		if (!HistoryMatchesExtent)
		{
			const float32 ConservativeDepth = 0.0f;
			for (uint32 Mip = 0; Mip < PyramidMipCount; ++Mip)
				glClearTexImage(PreviousHierarchicalDepth, static_cast<GLint>(Mip), GL_RED, GL_FLOAT, &ConservativeDepth);
		}
		glBindTextureUnit(0, PreviousHierarchicalDepth);
		if (CandidateCount == 0 || BatchCount == 0)
			return;
		Pipelines.VisibilityCull.SetUniformUInt(pipeline::shader::ComputeUniform::CandidateCount, CandidateCount);
		Pipelines.VisibilityCull.SetUniformUInt(pipeline::shader::ComputeUniform::PyramidMipCount, PyramidMipCount);
		Pipelines.VisibilityCull.SetUniformUInt(pipeline::shader::ComputeUniform::HistoryValid, HistoryMatchesExtent ? 1U : 0U);
		Pipelines.VisibilityCull.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, MaximumRenderItems);
		Pipelines.VisibilityCull.Bind();
		glDispatchCompute((CandidateCount + 63U) / 64U, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
		const uint32 VisibilityScanBlockCount = (BatchCount + 255U) / 256U;
		Pipelines.VisibilityPrefixScan.SetUniformUInt(pipeline::shader::ComputeUniform::BatchCount, BatchCount);
		Pipelines.VisibilityPrefixScan.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, MaximumRenderItems);
		Pipelines.VisibilityPrefixScan.Bind();
		glDispatchCompute(VisibilityScanBlockCount, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		Pipelines.VisibilityBlockPrefixScan.SetUniformUInt(pipeline::shader::ComputeUniform::BlockCount, VisibilityScanBlockCount);
		Pipelines.VisibilityBlockPrefixScan.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, MaximumRenderItems);
		Pipelines.VisibilityBlockPrefixScan.Bind();
		glDispatchCompute(1, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		Pipelines.VisibilityFinalize.SetUniformUInt(pipeline::shader::ComputeUniform::BatchCount, BatchCount);
		Pipelines.VisibilityFinalize.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, MaximumRenderItems);
		Pipelines.VisibilityFinalize.Bind();
		glDispatchCompute(VisibilityScanBlockCount, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		Pipelines.VisibilityScatter.SetUniformUInt(pipeline::shader::ComputeUniform::CandidateCount, CandidateCount);
		Pipelines.VisibilityScatter.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, MaximumRenderItems);
		Pipelines.VisibilityScatter.Bind();
		glDispatchCompute((CandidateCount + 63U) / 64U, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		return;
	}
	case pipeline::graph::HybridDeferredPassID::AccuratePicking:
	{
		Context.ValidateGraphicsPipelineTargets(Pipelines.AccuratePicking);
		const pipeline::graph::Extent2D PickExtent = Context.GetExtent(Resources.AccuratePickObjectID);
		const uint32 Background = pipeline::render::BackgroundPickID;
		const float32 FarDepth = 0.0f;
		struct ScissorScope final
		{
			ScissorScope()
			{
				glEnable(GL_SCISSOR_TEST);
			}
			~ScissorScope()
			{
				glDisable(GL_SCISSOR_TEST);
			}
		} Scope;
		for (const pipeline::render::ViewportPickPixel Pixel : this->PendingPickPixels)
		{
			if (Pixel.X >= PickExtent.Width || Pixel.Y >= PickExtent.Height)
				throw std::out_of_range("Accurate viewport pick coordinate is outside the render extent");
			glScissor(static_cast<GLint>(Pixel.X), static_cast<GLint>(Pixel.Y), 1, 1);
			glClearBufferuiv(GL_COLOR, 0, &Background);
			glClearBufferfv(GL_DEPTH, 0, &FarDepth);
			this->ExecuteHybridDrawBatches(Context, Pipelines.AccuratePicking, pipeline::render::RenderPassClass::GBuffer);
			this->ExecuteHybridDrawBatches(Context, Pipelines.AccuratePicking, pipeline::render::RenderPassClass::Transparency);
		}
		return;
	}
	case pipeline::graph::HybridDeferredPassID::DepthPrepass:
		this->ExecuteHybridDrawBatches(Context, Pipelines.DepthPrepass, pipeline::render::RenderPassClass::GBuffer);
		return;
	case pipeline::graph::HybridDeferredPassID::HierarchicalDepth:
	{
		const GLuint DepthTexture = Context.GetTexture(Resources.Depth);
		const GLuint PyramidTexture = Context.GetTexture(Resources.HierarchicalDepthWrite);
		const uint32 MipCount = static_cast<uint32>(std::bit_width(std::max(Extent.Width, Extent.Height)));
		Pipelines.HierarchicalDepth.Bind();
		for (uint32 Mip = 0; Mip < MipCount; ++Mip)
		{
			const uint32 OutputWidth = std::max(1U, Extent.Width >> Mip);
			const uint32 OutputHeight = std::max(1U, Extent.Height >> Mip);
			const uint32 SourceWidth = Mip == 0 ? Extent.Width : std::max(1U, Extent.Width >> (Mip - 1U));
			const uint32 SourceHeight = Mip == 0 ? Extent.Height : std::max(1U, Extent.Height >> (Mip - 1U));
			glBindTextureUnit(0, Mip == 0 ? DepthTexture : PyramidTexture);
			glBindImageTexture(0, PyramidTexture, static_cast<GLint>(Mip), GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
			Pipelines.HierarchicalDepth.SetUniformUInt2(pipeline::shader::ComputeUniform::SourceExtent, SourceWidth, SourceHeight);
			Pipelines.HierarchicalDepth.SetUniformUInt(pipeline::shader::ComputeUniform::SourceMip, Mip == 0 ? 0U : Mip - 1U);
			Pipelines.HierarchicalDepth.SetUniformUInt(pipeline::shader::ComputeUniform::SourceScale, Mip == 0 ? 1U : 2U);
			glDispatchCompute((OutputWidth + 7U) / 8U, (OutputHeight + 7U) / 8U, 1);
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
		}
		return;
	}
	case pipeline::graph::HybridDeferredPassID::GBuffer:
	{
		const uint32 Zero = 0;
		glClearTexImage(Context.GetTexture(Resources.Overdraw), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &Zero);
		glBindImageTexture(7, Context.GetTexture(Resources.Overdraw), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
		Pipelines.GBuffer.SetFragmentUniformUInt(
			pipeline::shader::FragmentUniform::TrackOverdraw,
			this->PendingViewportSettings.ViewMode == pipeline::render::ViewportViewMode::Overdraw ? 1U : 0U);
		this->ExecuteHybridDrawBatches(Context, Pipelines.GBuffer, pipeline::render::RenderPassClass::GBuffer);
		if (this->HeadlessPresentationValidation && this->HeadlessValidationFrameEligible && !this->PresentationValidated)
		{
			this->ValidateHeadlessDepthCoverage(Context.GetTexture(Resources.Depth), Context.GetExtent(Resources.Depth));
			this->ValidateHeadlessColorCoverage(Context.GetTexture(Resources.GBufferBaseColor),
												Context.GetExtent(Resources.GBufferBaseColor), "G-buffer base color");
			this->ValidateHeadlessColorCoverage(Context.GetTexture(Resources.GBufferNormalRoughness),
												Context.GetExtent(Resources.GBufferNormalRoughness), "G-buffer normal");
		}
		return;
	}
	case pipeline::graph::HybridDeferredPassID::ClusteredLights:
	{
		constexpr uint32 ActiveClusterCount = pipeline::render::ClusterCount;
		glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::FrameConstants),
						 Frame.FrameConstants.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Lights), Frame.Lights.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ClusterHeaders),
						 Context.GetBuffer(Resources.ClusterHeaders));
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ClusterIndices),
						 Context.GetBuffer(Resources.ClusterIndices));
		Pipelines.ClusteredLights.SetUniformUInt(pipeline::shader::ComputeUniform::LightCount,
												 static_cast<uint32>(this->LightBufferManager.GetTotalLightSourceCount()));
		Pipelines.ClusteredLights.SetUniformUInt(pipeline::shader::ComputeUniform::ClusterCount, ActiveClusterCount);
		Pipelines.ClusteredLights.Bind();
		glDispatchCompute((ActiveClusterCount + 63U) / 64U, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
		return;
	}
	case pipeline::graph::HybridDeferredPassID::DeferredLighting:
	{
		constexpr uint32 ActiveClusterCount = pipeline::render::ClusterCount;
		glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::FrameConstants),
						 Frame.FrameConstants.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Lights), Frame.Lights.Buffer);
		glBindTextureUnit(0, Context.GetTexture(Resources.GBufferBaseColor));
		glBindTextureUnit(1, Context.GetTexture(Resources.GBufferNormalRoughness));
		glBindTextureUnit(2, Context.GetTexture(Resources.GBufferMaterial));
		glBindTextureUnit(3, Context.GetTexture(Resources.Depth));
		glBindTextureUnit(4, Context.GetTexture(Resources.DirectionalShadowAtlas));
		glBindTextureUnit(5, Context.GetTexture(Resources.SpotShadowAtlas));
		glBindTextureUnit(6, Context.GetTexture(Resources.PointShadowArray));
		glBindTextureUnit(7, Context.GetTexture(Resources.GBufferOcclusion));
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ShadowData),
						 Frame.ShadowData.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ClusterHeaders),
						 Context.GetBuffer(Resources.ClusterHeaders));
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ClusterIndices),
						 Context.GetBuffer(Resources.ClusterIndices));
		Pipelines.DeferredLighting.SetUniformUInt(pipeline::shader::ComputeUniform::LightCount,
												  static_cast<uint32>(this->LightBufferManager.GetTotalLightSourceCount()));
		Pipelines.DeferredLighting.SetUniformUInt(pipeline::shader::ComputeUniform::ClusterCount, ActiveClusterCount);
		glBindImageTexture(0, Context.GetTexture(Resources.HDRLighting), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		this->ExecuteHybridDispatch(Pipelines.DeferredLighting, Context, Resources.HDRLighting);
		if (this->HeadlessPresentationValidation && this->HeadlessValidationFrameEligible)
			this->ValidateHeadlessColorCoverage(Context.GetTexture(Resources.HDRLighting), Context.GetExtent(Resources.HDRLighting),
												"deferred HDR");
		return;
	}
	case pipeline::graph::HybridDeferredPassID::WeightedOIT:
	{
		constexpr uint32 ActiveClusterCount = pipeline::render::ClusterCount;
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Lights), Frame.Lights.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ShadowData),
						 Frame.ShadowData.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ClusterHeaders),
						 Context.GetBuffer(Resources.ClusterHeaders));
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::ClusterIndices),
						 Context.GetBuffer(Resources.ClusterIndices));
		glBindTextureUnit(4, Context.GetTexture(Resources.DirectionalShadowAtlas));
		glBindTextureUnit(5, Context.GetTexture(Resources.SpotShadowAtlas));
		glBindTextureUnit(6, Context.GetTexture(Resources.PointShadowArray));
		glBindTextureUnit(7, Context.GetTexture(Resources.HDRLighting));
		Pipelines.TransparentOIT.SetFragmentUniformUInt(pipeline::shader::FragmentUniform::LightCount,
														static_cast<uint32>(this->LightBufferManager.GetTotalLightSourceCount()));
		Pipelines.TransparentOIT.SetFragmentUniformUInt(pipeline::shader::FragmentUniform::ClusterCount, ActiveClusterCount);
		glBindImageTexture(7, Context.GetTexture(Resources.Overdraw), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
		Pipelines.TransparentOIT.SetFragmentUniformUInt(
			pipeline::shader::FragmentUniform::TrackOverdraw,
			this->PendingViewportSettings.ViewMode == pipeline::render::ViewportViewMode::Overdraw ? 1U : 0U);
		this->ExecuteHybridDrawBatches(Context, Pipelines.TransparentOIT, pipeline::render::RenderPassClass::Transparency);
		return;
	}
	case pipeline::graph::HybridDeferredPassID::OITComposition:
		glBindTextureUnit(0, Context.GetTexture(Resources.HDRLighting));
		glBindTextureUnit(1, Context.GetTexture(Resources.TransparencyAccumulation));
		glBindTextureUnit(2, Context.GetTexture(Resources.TransparencyRevealage));
		glBindImageTexture(0, Context.GetTexture(Resources.CompositedHDR), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		this->ExecuteHybridDispatch(Pipelines.OITComposition, Context, Resources.CompositedHDR);
		return;
	case pipeline::graph::HybridDeferredPassID::TemporalAA:
	{
		const pipeline::graph::Extent2D ViewExtent = Context.GetExtent(Resources.TAAResolved);
		const bool HistoryMatchesExtent = View.TemporalHistoryValid && View.TemporalHistoryExtent.Width == ViewExtent.Width &&
										  View.TemporalHistoryExtent.Height == ViewExtent.Height;
		glBindTextureUnit(0, Context.GetTexture(Resources.CompositedHDR));
		glBindTextureUnit(1, Context.GetTexture(Resources.TAAHistoryRead));
		glBindTextureUnit(2, Context.GetTexture(Resources.Velocity));
		glBindTextureUnit(3, Context.GetTexture(Resources.ObjectID));
		glBindTextureUnit(4, Context.GetTexture(Resources.TAAObjectIDHistoryRead));
		Pipelines.TemporalAA.SetUniformUInt(pipeline::shader::ComputeUniform::HistoryValid, HistoryMatchesExtent ? 1U : 0U);
		glBindImageTexture(0, Context.GetTexture(Resources.TAAHistoryWrite), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		glBindImageTexture(1, Context.GetTexture(Resources.TAAResolved), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		glBindImageTexture(2, Context.GetTexture(Resources.TAAObjectIDHistoryWrite), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
		this->ExecuteHybridDispatch(Pipelines.TemporalAA, Context, Resources.TAAResolved);
		if (this->HeadlessPresentationValidation && this->HeadlessValidationFrameEligible)
			this->ValidateHeadlessColorCoverage(Context.GetTexture(Resources.TAAResolved), ViewExtent, "TAA resolve");
		return;
	}
	case pipeline::graph::HybridDeferredPassID::ViewportVisualization:
		glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::FrameConstants),
						 Frame.FrameConstants.Buffer);
		glBindTextureUnit(0, Context.GetTexture(Resources.TAAResolved));
		glBindTextureUnit(1, Context.GetTexture(Resources.GBufferBaseColor));
		glBindTextureUnit(2, Context.GetTexture(Resources.GBufferNormalRoughness));
		glBindTextureUnit(3, Context.GetTexture(Resources.GBufferMaterial));
		glBindTextureUnit(4, Context.GetTexture(Resources.Depth));
		glBindTextureUnit(5, Context.GetTexture(Resources.ObjectID));
		glBindTextureUnit(6, Context.GetTexture(Resources.Overdraw));
		glBindImageTexture(0, Context.GetTexture(Resources.VisualizedHDR), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		Pipelines.ViewportVisualization.SetUniformUInt(pipeline::shader::ComputeUniform::ViewMode,
													   static_cast<uint32>(this->PendingViewportSettings.ViewMode));
		this->ExecuteHybridDispatch(Pipelines.ViewportVisualization, Context, Resources.VisualizedHDR);
		return;
	case pipeline::graph::HybridDeferredPassID::SelectionOutline:
		glBindTextureUnit(0, Context.GetTexture(Resources.VisualizedHDR));
		glBindTextureUnit(1, Context.GetTexture(Resources.ObjectID));
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::SelectionMask),
						 Frame.SelectionMask.Buffer);
		glBindImageTexture(0, Context.GetTexture(Resources.OutlinedHDR), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		Pipelines.SelectionOutline.SetUniformUInt(pipeline::shader::ComputeUniform::SelectionWordCount,
												  this->PendingSelectionMask.GetWordCount());
		Pipelines.SelectionOutline.SetUniformUInt(pipeline::shader::ComputeUniform::OutlineRadius, this->SelectionOutlineRadius);
		Pipelines.SelectionOutline.SetUniformFloat4(pipeline::shader::ComputeUniform::OutlineColor, this->SelectionOutlineColor.r,
													this->SelectionOutlineColor.g, this->SelectionOutlineColor.b,
													this->SelectionOutlineColor.a);
		this->ExecuteHybridDispatch(Pipelines.SelectionOutline, Context, Resources.OutlinedHDR);
		return;
	case pipeline::graph::HybridDeferredPassID::WireframeOverlay:
		Context.ValidateGraphicsPipelineTargets(Pipelines.EditorWireframe);
		if (this->PendingViewportSettings.ViewMode != pipeline::render::ViewportViewMode::Wireframe)
			return;
		this->ExecuteHybridDrawBatches(Context, Pipelines.EditorWireframe, pipeline::render::RenderPassClass::GBuffer);
		return;
	case pipeline::graph::HybridDeferredPassID::EditorGrid:
		Context.ValidateGraphicsPipelineTargets(Pipelines.EditorGrid);
		if (!this->PendingViewportSettings.Overlays.Grid)
			return;
		Pipelines.EditorGrid.Bind();
		glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::FrameConstants),
						 Frame.FrameConstants.Buffer);
		glBindVertexArray(this->FullscreenVertexArray);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		return;
	case pipeline::graph::HybridDeferredPassID::DebugOverlay:
		Context.ValidateGraphicsPipelineTargets(Pipelines.EditorDebugLine);
		if (this->PendingDebugLines.empty())
			return;
		Pipelines.EditorDebugLine.Bind();
		glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::FrameConstants),
						 Frame.FrameConstants.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::DebugLines),
						 Frame.DebugLines.Buffer);
		glBindVertexArray(this->FullscreenVertexArray);
		glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(this->PendingDebugLines.size() * 6U));
		return;
	case pipeline::graph::HybridDeferredPassID::GizmoOverlay:
		Context.ValidateGraphicsPipelineTargets(Pipelines.GizmoOverlay);
		if (!this->PendingGizmoOverlay.has_value() || !this->PendingGizmoOverlay->Visible)
			return;
		{
			const pipeline::render::TransformGizmoOverlay &Overlay = *this->PendingGizmoOverlay;
			Pipelines.GizmoOverlay.SetVertexUniformFloat3(pipeline::shader::VertexUniform::GizmoPivot, Overlay.Pivot);
			Pipelines.GizmoOverlay.SetVertexUniformMatrix3(pipeline::shader::VertexUniform::GizmoBasis, Overlay.Basis);
			Pipelines.GizmoOverlay.SetVertexUniformFloat(pipeline::shader::VertexUniform::GizmoScale, Overlay.WorldScale);
			Pipelines.GizmoOverlay.SetVertexUniformUInt(pipeline::shader::VertexUniform::GizmoOperation, Overlay.Operation);
			Pipelines.GizmoOverlay.SetVertexUniformUInt(pipeline::shader::VertexUniform::ActiveHandle, Overlay.ActiveHandle);
			Pipelines.GizmoOverlay.Bind();
			glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::FrameConstants),
							 Frame.FrameConstants.Buffer);
			glBindVertexArray(this->FullscreenVertexArray);
			const GLsizei VertexCount = Overlay.Operation == 1U	  ? static_cast<GLsizei>(48U * 4U * 6U)
										: Overlay.Operation == 3U ? static_cast<GLsizei>((21U + 48U * 4U + 28U) * 6U)
																  : static_cast<GLsizei>(31U * 6U);
			glDrawArrays(GL_TRIANGLES, 0, VertexCount);
		}
		return;
	case pipeline::graph::HybridDeferredPassID::ExposureAndBloom:
	{
		const pipeline::graph::Extent2D BloomExtent = Context.GetExtent(Resources.Bloom);
		const uint32 MipCount = static_cast<uint32>(std::bit_width(std::max(BloomExtent.Width, BloomExtent.Height)));
		const GLuint BloomTexture = Context.GetTexture(Resources.Bloom);
		Pipelines.AutoExposure.Bind();
		glBindTextureUnit(0, Context.GetTexture(Resources.OutlinedHDR));
		glBindImageTexture(0, Context.GetTexture(Resources.Exposure), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
		Pipelines.AutoExposure.SetUniformUInt(pipeline::shader::ComputeUniform::HistoryValid, View.ExposureHistoryValid ? 1U : 0U);
		Pipelines.AutoExposure.SetUniformFloat(pipeline::shader::ComputeUniform::DeltaSeconds, View.FrameDeltaSeconds);
		glDispatchCompute(1, 1, 1);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
		Pipelines.Bloom.Bind();
		for (uint32 Mip = 0; Mip < MipCount; ++Mip)
		{
			const uint32 Width = std::max(1U, BloomExtent.Width >> Mip);
			const uint32 Height = std::max(1U, BloomExtent.Height >> Mip);
			glBindTextureUnit(0, Mip == 0 ? Context.GetTexture(Resources.OutlinedHDR) : BloomTexture);
			glBindImageTexture(0, BloomTexture, static_cast<GLint>(Mip), GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
			Pipelines.Bloom.SetUniformUInt(pipeline::shader::ComputeUniform::SourceMip, Mip == 0 ? 0U : Mip - 1U);
			Pipelines.Bloom.SetUniformUInt(pipeline::shader::ComputeUniform::Operation, 0U);
			glDispatchCompute((Width + 7U) / 8U, (Height + 7U) / 8U, 1);
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
		}
		for (uint32 Mip = MipCount - 1U; Mip > 0U; --Mip)
		{
			const uint32 OutputMip = Mip - 1U;
			const uint32 Width = std::max(1U, BloomExtent.Width >> OutputMip);
			const uint32 Height = std::max(1U, BloomExtent.Height >> OutputMip);
			glBindTextureUnit(0, BloomTexture);
			glBindImageTexture(0, BloomTexture, static_cast<GLint>(OutputMip), GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
			Pipelines.Bloom.SetUniformUInt(pipeline::shader::ComputeUniform::SourceMip, Mip);
			Pipelines.Bloom.SetUniformUInt(pipeline::shader::ComputeUniform::Operation, 1U);
			glDispatchCompute((Width + 7U) / 8U, (Height + 7U) / 8U, 1);
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
		}
		return;
	}
	case pipeline::graph::HybridDeferredPassID::ToneMapAndPresent:
		Context.ValidateGraphicsPipelineTargets(Pipelines.ToneMap);
		glBindTextureUnit(0, Context.GetTexture(Resources.OutlinedHDR));
		glBindTextureUnit(1, Context.GetTexture(Resources.Bloom));
		glBindTextureUnit(2, Context.GetTexture(Resources.Exposure));
		Pipelines.ToneMap.Bind();
		glBindVertexArray(this->FullscreenVertexArray);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		return;
	default:
		throw std::invalid_argument("Hybrid deferred pass identifier is invalid");
	}
}

Renderer::~Renderer()
{
	this->FrameResources.reset();
	if (this->Device)
	{
		this->Device->RetireGPUObject(pipeline::device::GPUObjectType::Framebuffer, ShadowFramebuffer);
		this->Device->RetireGPUObject(pipeline::device::GPUObjectType::Framebuffer, PresentationFramebuffer);
		this->Device->RetireGPUObject(pipeline::device::GPUObjectType::VertexArray, this->FullscreenVertexArray);
	}
	ShadowFramebuffer = 0;
	PresentationFramebuffer = 0;
	this->FullscreenVertexArray = 0;
}

uint32 Renderer::GetDrawCount() const noexcept
{
	return DrawCount;
}
uint32 Renderer::GetObjectsDrawn() const noexcept
{
	return ObjectsDrawn;
}

void Renderer::SetBackgroundColor(const glm::vec3 &Color)
{
	this->RequireOwnerThread();
	if (!std::isfinite(Color.x) || !std::isfinite(Color.y) || !std::isfinite(Color.z) || glm::any(glm::lessThan(Color, glm::vec3(0.0f))))
	{
		throw std::invalid_argument("Renderer background color must contain finite non-negative linear values");
	}
	this->BackgroundColor = Color;
}

const glm::vec3 &Renderer::GetBackgroundColor() const noexcept
{
	return this->BackgroundColor;
}

void Renderer::SetSelectionOutline(const glm::vec4 &Color, const uint32 Radius)
{
	this->RequireOwnerThread();
	const bool Finite = std::isfinite(Color.r) && std::isfinite(Color.g) && std::isfinite(Color.b) && std::isfinite(Color.a);
	if (!Finite || glm::any(glm::lessThan(Color, glm::vec4(0.0f))) || glm::any(glm::greaterThan(Color, glm::vec4(1.0f))))
		throw std::invalid_argument("Selection outline color must contain finite normalized values");
	if (Radius == 0 || Radius > 8)
		throw std::invalid_argument("Selection outline radius must be between one and eight pixels");
	this->SelectionOutlineColor = Color;
	this->SelectionOutlineRadius = Radius;
}

const glm::vec4 &Renderer::GetSelectionOutlineColor() const noexcept
{
	return this->SelectionOutlineColor;
}

uint32 Renderer::GetSelectionOutlineRadius() const noexcept
{
	return this->SelectionOutlineRadius;
}

resource::GPURealizationBatchResult Renderer::GetLastGPURealizationBatch() const noexcept
{
	return this->LastGPURealizationBatch;
}

void Renderer::Render(const world::Scene &Scene, resource::AssetManager &Assets, const Camera &Camera, const RenderViewID View,
					  const std::span<const world::ObjectHandle> SelectedObjects,
					  std::optional<pipeline::render::TransformGizmoOverlay> GizmoOverlay,
					  const pipeline::render::ViewportSettings Settings,
					  const std::span<const pipeline::render::ViewportPickPixel> PickPixels)
{
	const SceneRenderSnapshotBuildOptions Options{.IncludeBounds = Settings.Overlays.Bounds || Settings.Overlays.Culling,
												  .IncludeSkeletons = Settings.Overlays.Skeletons,
												  .IncludeCameras = Settings.Overlays.Cameras,
												  .IncludeLights = Settings.Overlays.Lights};
	SceneRenderSnapshotBuilder::BuildInto(Scene, this->SceneSnapshotScratch, Options, this->SceneSnapshotBuildScratch);
	this->Render(this->SceneSnapshotScratch, Assets, Camera, View, SelectedObjects, std::move(GizmoOverlay), Settings, PickPixels);
}

[[nodiscard]] bool IntersectsViewFrustum(const SceneDebugBounds &Bounds, const glm::mat4 &ViewProjection) noexcept
{
	std::array<glm::vec4, 8> Clip;
	for (uint32 Index = 0; Index < Bounds.Corners.size(); ++Index)
		Clip[Index] = ViewProjection * glm::vec4(Bounds.Corners[Index], 1.0f);
	const auto AllOutside = [&Clip](const auto &Predicate) { return std::ranges::all_of(Clip, Predicate); };
	return !AllOutside([](const glm::vec4 Value) { return Value.x < -Value.w; }) &&
		   !AllOutside([](const glm::vec4 Value) { return Value.x > Value.w; }) &&
		   !AllOutside([](const glm::vec4 Value) { return Value.y < -Value.w; }) &&
		   !AllOutside([](const glm::vec4 Value) { return Value.y > Value.w; }) &&
		   !AllOutside([](const glm::vec4 Value) { return Value.z < 0.0f; }) &&
		   !AllOutside([](const glm::vec4 Value) { return Value.z > Value.w; });
}

void AppendDebugBounds(std::vector<GPUDebugLineRecord> &Output, const SceneDebugBounds &Bounds, const glm::vec4 Color, const float32 Width)
{
	static constexpr std::array<std::array<uint8, 2>, 12> Edges{
		{{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
	if (Output.size() > Output.capacity() || Output.capacity() - Output.size() < Edges.size())
		throw std::overflow_error("Viewport debug-line capacity exceeded while appending bounds");
	for (const auto &Edge : Edges)
	{
		Output.push_back(
			{.StartAndWidth = glm::vec4(Bounds.Corners[Edge[0]], Width), .End = glm::vec4(Bounds.Corners[Edge[1]], 1.0f), .Color = Color});
	}
}

void Renderer::Render(const SceneRenderSnapshot &Snapshot, resource::AssetManager &Assets, const Camera &Camera, const RenderViewID View,
					  const std::span<const world::ObjectHandle> SelectedObjects,
					  std::optional<pipeline::render::TransformGizmoOverlay> GizmoOverlay,
					  const pipeline::render::ViewportSettings Settings,
					  const std::span<const pipeline::render::ViewportPickPixel> PickPixels)
{
	this->RequireOwnerThread();
	if (CollectingFrame)
		throw std::logic_error("Renderer accepts one Scene extraction per frame");
	if (!View.IsValid())
		throw std::invalid_argument("Renderer requires a valid render-view identity");
	if (Settings.ViewMode >= pipeline::render::ViewportViewMode::Count)
		throw std::invalid_argument("Renderer received an invalid viewport view mode");
	if (PickPixels.size() > this->PendingPickPixels.capacity())
		throw std::overflow_error("Viewport pick-request capacity exceeded");
	if (Snapshot.DebugLines.size() > this->PendingDebugLines.capacity())
		throw std::overflow_error("Viewport debug-line capacity exceeded");
	if (Snapshot.DebugBounds.size() > this->PendingDebugBounds.capacity())
		throw std::overflow_error("Viewport debug-bounds capacity exceeded");
	// Keep render-thread asset publication bounded. Resources that are not ready
	// use the extractor's explicit null-texture/skip fallback and remain queued.
	this->LastGPURealizationBatch =
		Assets.ProcessPendingGPU(this->Device.Get(), {.MaximumAssets = 32, .MaximumWallTime = std::chrono::microseconds(2'000)});
	(void)this->MeshGPUCache.ProcessPending(8, std::chrono::microseconds(2'000), this->FrameNumber);
	SceneCollection.BeginFrame(FrameNumber);
	ViewState &State = this->Views[View.Value];
	State.Settings = Settings;
	if (State.PreviousRenderTransforms.bucket_count() == 0)
	{
		State.PreviousRenderTransforms.reserve(MaximumRenderItems);
		State.CurrentRenderTransforms.reserve(MaximumRenderItems);
	}
	++State.RenderTransformGeneration;
	if (State.RenderTransformGeneration == 0)
	{
		State.PreviousRenderTransforms.clear();
		State.CurrentRenderTransforms.clear();
		State.RenderTransformGeneration = 1;
		State.PreviousRenderTransformGeneration = 0;
	}
	// Keep both history maps populated between frames. Current transforms are
	// overwritten below and generation-tagged stale keys are ignored; retaining
	// their nodes prevents per-frame allocation when a scene is rendered repeatedly.
	pipeline::render::SceneGpuResolver Extractor(this->Device.Get(), this->MeshGPUCache, Assets, State.PreviousRenderTransforms,
												 State.PreviousRenderTransformGeneration, State.CurrentRenderTransforms,
												 State.RenderTransformGeneration, this->SceneExtractorScratchStorage);
	Extractor.Extract(Snapshot, Camera, SceneCollection);
	const std::shared_ptr<const pipeline::render::FramePickTable> PickTable = SceneCollection.GetPickTable();
	if (PickTable == nullptr)
		throw std::logic_error("Scene extraction did not produce its frame pick table");
	this->PendingSelectionMask.BuildInto(*PickTable,
										 Settings.Overlays.Selection ? SelectedObjects : std::span<const world::ObjectHandle>{});
	this->PendingGizmoOverlay = std::move(GizmoOverlay);
	this->PendingViewportSettings = Settings;
	// Keep the submitted pixel order intact. EditorViewportRenderer uses this
	// order to associate each asynchronous readback with its request identity;
	// sorting or deduplicating here would silently remap out-of-order clicks.
	this->PendingPickPixels.assign(PickPixels.begin(), PickPixels.end());
	this->PendingDebugLines.clear();
	const auto CategoryEnabled = [&Settings](const SceneDebugLineCategory Category)
	{
		switch (Category)
		{
		case SceneDebugLineCategory::Bounds:
			return Settings.Overlays.Bounds;
		case SceneDebugLineCategory::Skeleton:
			return Settings.Overlays.Skeletons;
		case SceneDebugLineCategory::Camera:
			return Settings.Overlays.Cameras;
		case SceneDebugLineCategory::Light:
			return Settings.Overlays.Lights;
		}
		return false;
	};
	for (const SceneDebugLine &Line : Snapshot.DebugLines)
	{
		if (!CategoryEnabled(Line.Category))
			continue;
		this->PendingDebugLines.push_back(
			{.StartAndWidth = glm::vec4(Line.Start, 1.25f), .End = glm::vec4(Line.End, 1.0f), .Color = Line.Color});
	}
	if (Settings.Overlays.Culling)
		this->PendingDebugBounds.assign(Snapshot.DebugBounds.begin(), Snapshot.DebugBounds.end());
	else
		this->PendingDebugBounds.clear();
	if (this->PendingDebugLines.size() > MaximumDebugLines)
		throw std::overflow_error("Viewport debug-line capacity exceeded");
	State.PreviousRenderTransforms.swap(State.CurrentRenderTransforms);
	State.PreviousRenderTransformGeneration = State.RenderTransformGeneration;
	if (State.PreviousRenderTransforms.size() > static_cast<usize>(MaximumRenderItems) * 2U)
	{
		const uint64 PublishedGeneration = State.PreviousRenderTransformGeneration;
		std::erase_if(State.PreviousRenderTransforms,
					  [PublishedGeneration](const auto &Entry) { return Entry.second.Generation != PublishedGeneration; });
	}
	this->ObjectsDrawn = Snapshot.ObjectCount;
	this->PendingView = View;
	CollectingFrame = true;
}

void Renderer::UploadFrameConstants(const Camera &Camera, const core::WindowExtent Extent, pipeline::frame::FrameResources &FrameResources,
									ViewState &State)
{
	const auto CurrentTime = std::chrono::steady_clock::now();
	if (State.LastFrameTime != std::chrono::steady_clock::time_point{})
	{
		const float32 Elapsed = std::chrono::duration<float32>(CurrentTime - State.LastFrameTime).count();
		State.FrameDeltaSeconds = std::clamp(Elapsed, 1.0f / 1'000.0f, 0.25f);
	}
	State.LastFrameTime = CurrentTime;
	const glm::mat4 Projection = Camera.GetProjectionMatrix(Extent);
	const glm::mat4 View = Camera.GetViewMatrix();
	const glm::mat4 ViewProjection = Projection * View;
	const pipeline::render::GPUFrameConstants Frame{
		.Projection = Projection,
		.View = View,
		.ViewProjection = ViewProjection,
		.PreviousViewProjection = State.PreviousViewProjection,
		.InverseViewProjection = glm::inverse(ViewProjection),
		.CameraPositionAndNear = glm::vec4(Camera.Position, Camera.NearPlane),
		.RenderExtentAndFar =
			glm::vec4(static_cast<float32>(Extent.Width), static_cast<float32>(Extent.Height), Camera.FarPlane, State.FrameDeltaSeconds),
		.CountsAndFrame = glm::uvec4(0, 0, 0, static_cast<uint32>(FrameNumber)),
		.BackgroundColor = glm::vec4(this->BackgroundColor, 1.0f)};
	CopyToMappedBuffer(FrameResources.FrameConstants, &Frame, sizeof(Frame), "frame constants");
	glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::FrameConstants),
					 FrameResources.FrameConstants.Buffer);
	State.PreviousViewProjection = Frame.ViewProjection;
}

void Renderer::PresentView(const RenderViewOutput &Output, core::Window &Window)
{
	this->RequireOwnerThread();
	if (!Output.IsValid())
		throw std::invalid_argument("Renderer presentation requires a valid render-view output");
	const auto View = this->Views.find(Output.View.Value);
	if (View == this->Views.end() || View->second.Generation != Output.Generation || View->second.Extent != Output.Extent ||
		View->second.LastOutputFrameNumber != Output.FrameNumber)
		throw std::logic_error("Renderer presentation received a stale render-view output");
	const core::WindowExtent Extent = Window.GetFramebufferExtent();
	if (!Extent.IsValid())
	{
		if (this->HeadlessPresentationValidation && this->HeadlessValidationFrameEligible)
			throw std::runtime_error("Headless presentation validation received a zero-sized framebuffer");
		return;
	}
	glNamedFramebufferTexture(this->PresentationFramebuffer, GL_COLOR_ATTACHMENT0, Output.Color.Texture, 0);
	glNamedFramebufferReadBuffer(this->PresentationFramebuffer, GL_COLOR_ATTACHMENT0);
	if (!this->PresentationFramebufferState.Valid || this->PresentationFramebufferState.Texture != Output.Color.Texture ||
		this->PresentationFramebufferState.View != Output.View || this->PresentationFramebufferState.Generation != Output.Generation ||
		this->PresentationFramebufferState.Extent != Output.Color.Extent)
	{
		if (glCheckNamedFramebufferStatus(this->PresentationFramebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			throw std::runtime_error("Renderer presentation framebuffer is incomplete");
		this->PresentationFramebufferState = {.Texture = Output.Color.Texture,
											  .View = Output.View,
											  .Generation = Output.Generation,
											  .Extent = Output.Color.Extent,
											  .Valid = true};
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);
	glBlitNamedFramebuffer(this->PresentationFramebuffer, 0, 0, 0, static_cast<GLint>(Output.Color.Extent.Width),
						   static_cast<GLint>(Output.Color.Extent.Height), 0, 0, static_cast<GLint>(Extent.Width),
						   static_cast<GLint>(Extent.Height), GL_COLOR_BUFFER_BIT, GL_NEAREST);
	this->ValidateHeadlessPresentation(Window);
}

void Renderer::PresentView(const PresentationRequest &Request)
{
	if (!Request.IsValid())
		throw std::invalid_argument("Renderer presentation requires a valid request");
	this->PresentView(Request.Output, *Request.Window);
}

void Renderer::PrepareInterfacePresentation(core::Window &Window)
{
	this->RequireOwnerThread();
	const core::WindowExtent Extent = Window.GetFramebufferExtent();
	if (!Extent.IsValid())
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);
	glViewport(0, 0, static_cast<GLsizei>(Extent.Width), static_cast<GLsizei>(Extent.Height));
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	this->Device->InvalidateGraphicsPipelineStateCache();
	glClearColor(0.025f, 0.028f, 0.035f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

RenderViewOutput Renderer::RenderView(const pipeline::render::RenderPassPipelineSet &Pipelines, const Camera &Camera,
									  const core::WindowExtent Extent, const RenderViewID View)
{
	return this->RenderView(Pipelines, Camera, RenderViewDescriptor{.View = View, .Extent = Extent});
}

RenderViewOutput Renderer::RenderView(const pipeline::render::RenderPassPipelineSet &Pipelines, const Camera &Camera,
									  const RenderViewDescriptor &Descriptor)
{
	this->RequireOwnerThread();
	if (!Descriptor.IsValid())
		throw std::invalid_argument("Renderer requires a valid render-view descriptor");
	return this->RenderViewInternal(Pipelines, Camera, Descriptor.Extent, Descriptor.View);
}

RenderViewOutput Renderer::RenderViewInternal(const pipeline::render::RenderPassPipelineSet &Pipelines, const Camera &Camera,
											  const core::WindowExtent WindowExtent, const RenderViewID ViewID)
{
	if (!CollectingFrame)
		throw std::logic_error("Renderer view execution requires a preceding scene submission for the same frame");
	if (!ViewID.IsValid() || this->PendingView != ViewID)
		throw std::logic_error("Renderer view output must match the preceding scene extraction view");
	ViewState &View = this->Views.at(ViewID.Value);
	auto Recovery = MakeScopeExit([this]() noexcept { this->RecoverFailedFrame(); });
	if (!WindowExtent.IsValid())
		throw std::invalid_argument("Renderer cannot render a zero-sized view");
	if (View.Extent != WindowExtent)
	{
		View.Extent = WindowExtent;
		++View.Generation;
		if (View.Generation == 0)
			View.Generation = 1;
		View.HierarchicalDepthHistoryValid = false;
		View.TemporalHistoryValid = false;
		View.ExposureHistoryValid = false;
	}
	const glm::mat4 CurrentProjection = Camera.GetProjectionMatrix(WindowExtent);
	const auto ProjectionChanged = [&View, &CurrentProjection]() noexcept
	{
		if (!View.HasPreviousCameraState)
			return false;
		for (uint32 Column = 0; Column < 4U; ++Column)
		{
			for (uint32 Row = 0; Row < 4U; ++Row)
			{
				const float32 Previous = View.PreviousProjection[Column][Row];
				const float32 Current = CurrentProjection[Column][Row];
				const float32 Scale = std::max({1.0f, std::abs(Previous), std::abs(Current)});
				if (std::abs(Previous - Current) > Scale * 1.0e-5f)
					return true;
			}
		}
		return false;
	}();
	const bool CameraCut = View.HasPreviousCameraState &&
						   (glm::distance(Camera.Position, View.PreviousCameraPosition) > 2.0f ||
							glm::dot(glm::normalize(Camera.Front), glm::normalize(View.PreviousCameraFront)) < 0.95f ||
							glm::dot(glm::normalize(Camera.Up), glm::normalize(View.PreviousCameraUp)) < 0.999f || ProjectionChanged);
	if (CameraCut)
	{
		View.HierarchicalDepthHistoryValid = false;
		View.TemporalHistoryValid = false;
		View.ExposureHistoryValid = false;
	}
	this->LightBufferManager.UploadSceneLights(SceneCollection);
	SceneCollection.Seal();
	this->DrawCount = 0;
	this->ScenePreparation.PrepareInto(SceneCollection, CurrentProjection * Camera.GetViewMatrix(), 0, 0, this->MainPreparationWorkspace,
									   this->Prepared);
	this->ScenePreparation.PrepareInto(SceneCollection, glm::mat4(1.0f), 0, 0, this->ShadowPreparationWorkspace, this->ShadowPrepared,
									   false, true);
	const pipeline::render::RenderPreparationResult &Prepared = this->Prepared;
	const pipeline::render::RenderPreparationResult &ShadowPrepared = this->ShadowPrepared;
	this->HeadlessValidationFrameEligible = !Prepared.CandidateInstances.empty();
	const uint64 ShadowCasterSignature = CalculateShadowCasterSignature(SceneCollection);
	if (Prepared.CandidateInstances.size() > MaximumRenderItems || Prepared.CandidateCommands.size() > MaximumRenderItems ||
		ShadowPrepared.CandidateInstances.size() > MaximumRenderItems || ShadowPrepared.CandidateCommands.size() > MaximumRenderItems)
		throw std::runtime_error("Renderer frame capacity exceeded; increase MaximumRenderItems before submitting more geometry");
	if (!this->PendingDebugBounds.empty())
	{
		const glm::mat4 ViewProjection = Camera.GetProjectionMatrix(WindowExtent) * Camera.GetViewMatrix();
		for (const SceneDebugBounds &Bounds : this->PendingDebugBounds)
		{
			const bool Visible = IntersectsViewFrustum(Bounds, ViewProjection);
			AppendDebugBounds(this->PendingDebugLines, Bounds,
							  Visible ? glm::vec4(0.25f, 1.0f, 0.35f, 0.9f) : glm::vec4(1.0f, 0.2f, 0.15f, 0.9f), 1.5f);
		}
	}
	if (this->PendingDebugLines.size() > MaximumDebugLines)
		throw std::runtime_error("Renderer viewport debug-line capacity exceeded");

	pipeline::frame::FrameResources &Frame = FrameResources->Acquire(FrameNumber);
	this->UploadFrameConstants(Camera, WindowExtent, Frame, View);
	const std::span<const pipeline::render::GPULightRecord> LightRecords = this->LightBufferManager.GetGPURecords();
	CopyToMappedBuffer(Frame.Lights, LightRecords.data(), LightRecords.size_bytes(), "lights");
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Lights), Frame.Lights.Buffer);
	this->MeshGPUCache.Collect(FrameNumber, pipeline::frame::FrameResourceRing::FrameCount);
	if (SceneCollection.GetSkinningMatrices().size() > MaximumSkinMatrices)
		throw std::runtime_error("Renderer skinning palette capacity exceeded");
	CopyToMappedBuffer(Frame.SkinMatrices, SceneCollection.GetSkinningMatrices().data(),
					   SceneCollection.GetSkinningMatrices().size() * sizeof(pipeline::render::GPUSkinMatrixRecord), "skin matrices");
	if (SceneCollection.GetMorphWeights().size() > MaximumMorphWeights)
		throw std::runtime_error("Renderer morph weight capacity exceeded");
	CopyToMappedBuffer(Frame.MorphWeights, SceneCollection.GetMorphWeights().data(),
					   SceneCollection.GetMorphWeights().size() * sizeof(pipeline::render::GPUMorphWeightRecord), "morph weights");
	CopyToMappedBuffer(Frame.SelectionMask, this->PendingSelectionMask.GetWords().data(),
					   this->PendingSelectionMask.GetWords().size() * sizeof(uint32), "selection mask");
	CopyToMappedBuffer(Frame.DebugLines, this->PendingDebugLines.data(),
					   this->PendingDebugLines.size() * sizeof(pipeline::render::GPUDebugLineRecord), "viewport debug lines");
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::SelectionMask),
					 Frame.SelectionMask.Buffer);
	const uint64 InstanceBytes = static_cast<uint64>(Prepared.CandidateInstances.size() * sizeof(pipeline::render::PreparedInstance));
	if (InstanceBytes != 0)
	{
		CopyToMappedBuffer(Frame.CandidateInstances, Prepared.CandidateInstances.data(), InstanceBytes, "candidate instances");
		CopyToMappedBuffer(Frame.VisibleInstances, Prepared.CandidateInstances.data(), InstanceBytes, "visible instances");
	}
	const uint64 ShadowInstanceBytes =
		static_cast<uint64>(ShadowPrepared.CandidateInstances.size() * sizeof(pipeline::render::PreparedInstance));
	if (ShadowInstanceBytes != 0)
		CopyToMappedBuffer(Frame.ShadowInstances, ShadowPrepared.CandidateInstances.data(), ShadowInstanceBytes, "shadow instances");
	CopyToMappedBuffer(Frame.IndirectCommands, Prepared.CandidateCommands.data(),
					   Prepared.CandidateCommands.size() * sizeof(pipeline::render::RenderCommand), "indirect commands");
	CopyToMappedBuffer(Frame.BatchMetadata, Prepared.Batches.data(), Prepared.Batches.size() * sizeof(pipeline::render::RenderBatch),
					   "batch metadata");
	// Visibility compaction writes the final per-batch instance counts directly
	// into DrawElementsIndirectCommand. Each command starts empty every frame.
	for (uint32 CommandIndex = 0; CommandIndex < Prepared.CandidateCommands.size(); ++CommandIndex)
	{
		static_cast<pipeline::render::RenderCommand *>(Frame.IndirectCommands.MappedMemory)[CommandIndex].InstanceCount = 0;
	}
	if (Prepared.Materials.size() > MaximumRenderItems)
		throw std::runtime_error("Renderer material capacity exceeded; increase MaximumRenderItems before submitting more materials");
	CopyToMappedBuffer(Frame.Materials, Prepared.Materials.data(), Prepared.Materials.size() * sizeof(pipeline::render::GPUMaterialRecord),
					   "materials");
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(pipeline::render::RendererBinding::Materials), Frame.Materials.Buffer);
	glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

	uint32 DirectionalCascadeCount = 0;
	uint32 SpotShadowCount = 0;
	uint32 PointShadowFaceCount = 0;
	uint32 UnshadowedDirectionalLightRequests = 0;
	uint32 UnshadowedSpotLightRequests = 0;
	uint32 UnshadowedPointLightRequests = 0;
	const std::vector<DirectionalLightSource> &DirectionalLights = LightBufferManager.GetDirectionalLights();
	const DirectionalLightSource *ShadowDirectionalLight = nullptr;
	for (const DirectionalLightSource &Light : DirectionalLights)
	{
		if (Light.CastShadows <= 0.5f)
			continue;
		if (ShadowDirectionalLight == nullptr)
			ShadowDirectionalLight = &Light;
		else
			++UnshadowedDirectionalLightRequests;
	}
	if (ShadowDirectionalLight != nullptr)
	{
		constexpr std::array<float32, pipeline::render::DirectionalShadowCascadeCount> CascadeRadii{25.0f, 75.0f, 200.0f, 500.0f};
		const glm::vec3 Direction = glm::normalize(ShadowDirectionalLight->Direction);
		for (uint32 Cascade = 0; Cascade < pipeline::render::DirectionalShadowCascadeCount; ++Cascade)
		{
			const float32 Radius = CascadeRadii[Cascade];
			const glm::vec3 Center = Camera.Position + Camera.Front * (Radius * 0.5f);
			const glm::mat4 LightView =
				glm::lookAt(Center - Direction * (Radius * 2.0f), Center,
							glm::abs(Direction.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f));
			ShadowRecords[Cascade] = {.ViewProjection = glm::orthoRH_ZO(-Radius, Radius, -Radius, Radius, 0.1f, Radius * 4.0f) * LightView,
									  .AtlasScaleBias = glm::vec4(1.0f, 1.0f, static_cast<float32>(Cascade), 0.0f),
									  .DepthBiasAndFilter = glm::vec4(0.0015f, 0.0035f, Radius, 0.0f)};
		}
		DirectionalCascadeCount = pipeline::render::DirectionalShadowCascadeCount;
	}
	const std::vector<SpotLightSource> &SpotLights = LightBufferManager.GetSpotLights();
	for (const SpotLightSource &Light : SpotLights)
	{
		if (Light.CastShadows <= 0.5f)
			continue;
		if (SpotShadowCount >= pipeline::render::MaximumSpotShadowCount)
		{
			++UnshadowedSpotLightRequests;
			continue;
		}
		const uint32 SpotIndex = SpotShadowCount++;
		const glm::vec3 Direction = glm::normalize(Light.Direction);
		const glm::vec3 Up = glm::abs(Direction.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		const float32 Range = pipeline::lighting::LightBufferManager::CalculateInfluenceRange(Light);
		ShadowRecords[pipeline::render::DirectionalShadowCascadeCount + SpotIndex] = {
			.ViewProjection = glm::perspectiveRH_ZO(glm::acos(glm::clamp(Light.OuterCutOff, -1.0f, 1.0f)) * 2.0f, 1.0f, 0.1f, Range) *
							  glm::lookAt(Light.Position, Light.Position + Direction, Up),
			.AtlasScaleBias = glm::vec4(1.0f, 1.0f, static_cast<float32>(SpotIndex), 0.0f),
			.DepthBiasAndFilter = glm::vec4(0.001f, 0.003f, Range, 0.0f)};
	}
	const std::vector<PointLightSource> &PointLights = LightBufferManager.GetPointLights();
	uint32 PointShadowCount = 0;
	constexpr std::array<glm::vec3, 6> PointFaceDirections{glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
														   glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
														   glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
	constexpr std::array<glm::vec3, 6> PointFaceUps{glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
													glm::vec3(0.0f, 0.0f, 1.0f),  glm::vec3(0.0f, 0.0f, -1.0f),
													glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)};
	for (const PointLightSource &Light : PointLights)
	{
		if (Light.CastShadows <= 0.5f)
			continue;
		if (PointShadowCount >= pipeline::render::MaximumPointShadowCount)
		{
			++UnshadowedPointLightRequests;
			continue;
		}
		const uint32 PointIndex = PointShadowCount++;
		const float32 Range = pipeline::lighting::LightBufferManager::CalculateInfluenceRange(Light);
		for (uint32 Face = 0; Face < 6; ++Face)
		{
			const uint32 RecordIndex =
				pipeline::render::DirectionalShadowCascadeCount + pipeline::render::MaximumSpotShadowCount + PointIndex * 6U + Face;
			ShadowRecords[RecordIndex] = {.ViewProjection =
											  glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, Range) *
											  glm::lookAt(Light.Position, Light.Position + PointFaceDirections[Face], PointFaceUps[Face]),
										  .AtlasScaleBias = glm::vec4(1.0f, 1.0f, static_cast<float32>(PointIndex * 6U + Face), 0.0f),
										  .DepthBiasAndFilter = glm::vec4(0.002f, 0.004f, Range, 0.0f)};
		}
	}
	PointShadowFaceCount = PointShadowCount * 6U;
	CopyToMappedBuffer(Frame.ShadowData, ShadowRecords.data(), ShadowRecords.size() * sizeof(pipeline::render::GPUShadowRecord),
					   "shadow data");

	const pipeline::graph::Extent2D Extent{WindowExtent.Width, WindowExtent.Height};
	if (View.Graph == nullptr)
		View.Graph = std::make_unique<pipeline::graph::RenderGraph>(this->Device.Get());
	if (View.HistoryNamespace.empty())
		View.HistoryNamespace = "RenderView-" + std::to_string(ViewID.Value);
	pipeline::graph::RenderGraph &RenderGraph = *View.Graph;
	RenderGraph.BeginFrame(Extent, ViewID.Value, View.Generation);
	auto Import = [&RenderGraph](const string_view Name, const pipeline::frame::FrameBufferSlice &Slice)
	{ return RenderGraph.ImportBuffer(Name, Slice.CapacityInBytes, GL_DYNAMIC_STORAGE_BIT, Slice.Buffer); };
	const pipeline::graph::HybridDeferredFrameInputs Inputs{.Extent = Extent,
															.CandidateInstances = Import("CandidateInstances", Frame.CandidateInstances),
															.ShadowInstances = Import("ShadowInstances", Frame.ShadowInstances),
															.VisibleInstances = Import("VisibleInstances", Frame.VisibleInstances),
															.IndirectCommands = Import("IndirectCommands", Frame.IndirectCommands),
															.BatchMetadata = Import("BatchMetadata", Frame.BatchMetadata),
															.VisibilityScratch = Import("VisibilityScratch", Frame.VisibilityScratch),
															.SelectionMask = Import("SelectionMask", Frame.SelectionMask),
															.SelectionMaskWordCount = this->PendingSelectionMask.GetWordCount(),
															.DirectionalShadowLayerCount = DirectionalCascadeCount,
															.SpotShadowLayerCount = SpotShadowCount,
															.PointShadowFaceLayerCount = PointShadowFaceCount,
															.TemporalHistoryWriteIndex = static_cast<uint32>(View.FrameNumber & 1U),
															.HistoryNamespace = View.HistoryNamespace,
															.AccuratePickingEnabled = !this->PendingPickPixels.empty()};
	this->ActiveHybridPass = {.Pipelines = &Pipelines,
							  .Frame = &Frame,
							  .Prepared = &this->Prepared,
							  .ShadowPrepared = &this->ShadowPrepared,
							  .View = &View,
							  .Extent = Extent,
							  .DirectionalCascadeCount = DirectionalCascadeCount,
							  .SpotShadowCount = SpotShadowCount,
							  .PointShadowFaceCount = PointShadowFaceCount,
							  .ShadowCasterSignature = ShadowCasterSignature};
	const auto ClearHybridPassState = MakeScopeExit([this]() noexcept { this->ActiveHybridPass = {}; });
	(void)ClearHybridPassState;
	this->HybridFrameGraph.BuildInto(RenderGraph, Inputs, this->HybridPassCallbacks, View.HybridResources);
	const pipeline::graph::TextureHandle PickOutput =
		this->PendingPickPixels.empty() ? View.HybridResources.ObjectID : View.HybridResources.AccuratePickObjectID;
	RenderGraph.ExportTexture(View.HybridResources.Presentation);
	RenderGraph.ExportTexture(PickOutput);
	RenderGraph.ExportTexture(View.HybridResources.Depth);
	RenderGraph.Compile();
	RenderGraph.Execute();
	const usize InspectionIndex = static_cast<usize>(View.FrameNumber % GraphInspectionCacheSize);
	std::shared_ptr<pipeline::graph::RenderGraphInspection> &Inspection = View.GraphInspectionCache[InspectionIndex];
	if (Inspection == nullptr || Inspection.use_count() != 1)
		Inspection = std::make_shared<pipeline::graph::RenderGraphInspection>();
	RenderGraph.InspectInto(*Inspection);
	const RenderViewOutput Output{.View = ViewID,
								  .Extent = WindowExtent,
								  .Color = RenderGraph.GetExportedTexture(View.HybridResources.Presentation),
								  .ObjectID = RenderGraph.GetExportedTexture(PickOutput),
								  .Depth = RenderGraph.GetExportedTexture(View.HybridResources.Depth),
								  .Generation = View.Generation,
								  .FrameNumber = this->FrameNumber,
								  .PickTable = this->PendingPickPixels.empty() ? nullptr : SceneCollection.GetPickTable(),
								  .RenderStatistics = {.SubmittedObjects = this->ObjectsDrawn,
													   .CandidateInstances = static_cast<uint32>(Prepared.CandidateInstances.size()),
													   .RenderBatches = static_cast<uint32>(Prepared.Batches.size()),
													   .DrawCalls = this->DrawCount,
													   .DebugLines = static_cast<uint32>(this->PendingDebugLines.size()),
													   .UnshadowedDirectionalLightRequests = UnshadowedDirectionalLightRequests,
													   .UnshadowedPointLightRequests = UnshadowedPointLightRequests,
													   .UnshadowedSpotLightRequests = UnshadowedSpotLightRequests},
								  .GraphInspection = Inspection};
	View.LastOutputFrameNumber = this->FrameNumber;
	View.HierarchicalDepthHistoryExtent = Extent;
	View.HierarchicalDepthHistoryValid = true;
	View.TemporalHistoryExtent = Extent;
	View.TemporalHistoryValid = true;
	View.ExposureHistoryValid = true;
	View.PreviousCameraPosition = Camera.Position;
	View.PreviousCameraFront = Camera.Front;
	View.PreviousCameraUp = Camera.Up;
	View.PreviousProjection = CurrentProjection;
	View.HasPreviousCameraState = true;
	SceneCollection.ReleaseAssetPinsInto(this->AssetPinTransfer);
	FrameResources->Retire(this->AssetPinTransfer);
	++View.FrameNumber;
	++FrameNumber;
	CollectingFrame = false;
	this->PendingSelectionMask.Clear();
	this->PendingGizmoOverlay.reset();
	this->PendingViewportSettings = {};
	this->PendingPickPixels.clear();
	this->PendingDebugLines.clear();
	this->PendingDebugBounds.clear();
	SceneCollection.Clear();
	ObjectsDrawn = 0;
	Recovery.Release();
	return Output;
}

void Renderer::RecoverFailedFrame() noexcept
{
	this->SceneCollection.ReleaseAssetPinsInto(this->AssetPinTransfer);
	if (this->FrameResources != nullptr && this->FrameResources->IsAcquired())
	{
		try
		{
			this->FrameResources->Retire(this->AssetPinTransfer);
		}
		catch (...)
		{
			this->FrameResources->Abandon(std::move(this->AssetPinTransfer));
		}
	}
	else
	{
		this->AssetPinTransfer.clear();
	}
	this->CollectingFrame = false;
	this->PendingSelectionMask.Clear();
	this->PendingGizmoOverlay.reset();
	this->PendingViewportSettings = {};
	this->PendingPickPixels.clear();
	this->PendingDebugLines.clear();
	this->PendingDebugBounds.clear();
	this->SceneCollection.Clear();
	this->ObjectsDrawn = 0;
	const auto View = this->Views.find(this->PendingView.Value);
	if (View != this->Views.end())
	{
		View->second.HierarchicalDepthHistoryValid = false;
		View->second.TemporalHistoryValid = false;
		View->second.ExposureHistoryValid = false;
	}
}

void Renderer::InvalidateView(const RenderViewID View)
{
	this->RequireOwnerThread();
	if (!View.IsValid())
		throw std::invalid_argument("Cannot invalidate an empty render-view identity");
	ViewState &State = this->Views[View.Value];
	State.HierarchicalDepthHistoryValid = false;
	State.TemporalHistoryValid = false;
	State.ExposureHistoryValid = false;
	State.HasPreviousCameraState = false;
	++State.Generation;
	if (State.Generation == 0)
		State.Generation = 1;
	State.PreviousRenderTransforms.clear();
	State.CurrentRenderTransforms.clear();
	State.RenderTransformGeneration = 0;
	State.PreviousRenderTransformGeneration = 0;
}

std::optional<RenderViewState> Renderer::GetViewState(const RenderViewID View) const
{
	this->RequireOwnerThread();
	if (!View.IsValid())
		return std::nullopt;
	const auto Iterator = this->Views.find(View.Value);
	if (Iterator == this->Views.end())
		return std::nullopt;
	const ViewState &State = Iterator->second;
	return RenderViewState{.View = View,
						   .Extent = State.Extent,
						   .Settings = State.Settings,
						   .Generation = State.Generation,
						   .FrameNumber = State.FrameNumber,
						   .LastOutputFrameNumber = State.LastOutputFrameNumber,
						   .HierarchicalDepthHistoryValid = State.HierarchicalDepthHistoryValid,
						   .TemporalHistoryValid = State.TemporalHistoryValid,
						   .ExposureHistoryValid = State.ExposureHistoryValid,
						   .HasPreviousCameraState = State.HasPreviousCameraState};
}

void Renderer::ReleaseView(const RenderViewID View)
{
	this->RequireOwnerThread();
	if (!View.IsValid())
		return;
	if (this->CollectingFrame && this->PendingView == View)
		throw std::logic_error("Cannot release a render view while it has an active frame");
	this->Views.erase(View.Value);
}

void Renderer::EnableCulling() const
{
	this->RequireOwnerThread();
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	this->Device->InvalidateGraphicsPipelineStateCache();
}
void Renderer::DisableCulling() const
{
	this->RequireOwnerThread();
	glDisable(GL_CULL_FACE);
	this->Device->InvalidateGraphicsPipelineStateCache();
}

void Renderer::ValidateHeadlessDepthCoverage(GLuint DepthTexture, pipeline::graph::Extent2D Extent) const
{
	if (this->PresentationValidated)
		return;
	const uint64 PixelCount = static_cast<uint64>(Extent.Width) * static_cast<uint64>(Extent.Height);
	if (PixelCount == 0 || PixelCount > static_cast<uint64>(std::numeric_limits<usize>::max() / sizeof(float32)))
		throw std::runtime_error("Headless depth validation readback exceeds addressable memory");
	std::vector<float32> DepthValues(static_cast<usize>(PixelCount));
	glGetTextureSubImage(DepthTexture, 0, 0, 0, 0, static_cast<GLsizei>(Extent.Width), static_cast<GLsizei>(Extent.Height), 1,
						 GL_DEPTH_COMPONENT, GL_FLOAT, static_cast<GLsizei>(PixelCount * sizeof(float32)), DepthValues.data());
	this->Device->CheckErrors("Headless G-buffer depth validation");
	uint64 CoveredPixelCount = 0;
	for (const float32 Depth : DepthValues)
		if (Depth > 0.0001f)
			++CoveredPixelCount;
	if (CoveredPixelCount == 0)
		throw std::runtime_error("Headless validation found no opaque depth coverage after the G-buffer pass");
	std::cerr << "Headless G-buffer validation: " << CoveredPixelCount << " covered pixels\n";
}

void Renderer::ValidateHeadlessColorCoverage(GLuint ColorTexture, pipeline::graph::Extent2D Extent, string_view Stage) const
{
	if (this->PresentationValidated)
		return;
	const uint64 PixelCount = static_cast<uint64>(Extent.Width) * static_cast<uint64>(Extent.Height);
	if (PixelCount == 0 || PixelCount > static_cast<uint64>(std::numeric_limits<usize>::max() / (sizeof(float32) * 4U)))
		throw std::runtime_error("Headless color validation readback exceeds addressable memory");
	const uint64 ComponentCount = PixelCount * 4U;
	std::vector<float32> Values(static_cast<usize>(ComponentCount));
	glGetTextureSubImage(ColorTexture, 0, 0, 0, 0, static_cast<GLsizei>(Extent.Width), static_cast<GLsizei>(Extent.Height), 1, GL_RGBA,
						 GL_FLOAT, static_cast<GLsizei>(ComponentCount * sizeof(float32)), Values.data());
	this->Device->CheckErrors("Headless " + std::string(Stage) + " validation");
	uint64 LitPixelCount = 0;
	for (uint64 Offset = 0; Offset < ComponentCount; Offset += 4U)
	{
		if (Values[static_cast<usize>(Offset)] > 0.001f || Values[static_cast<usize>(Offset + 1U)] > 0.001f ||
			Values[static_cast<usize>(Offset + 2U)] > 0.001f)
			++LitPixelCount;
	}
	if (LitPixelCount == 0)
		throw std::runtime_error("Headless validation found no visible color in " + std::string(Stage));
	std::cerr << "Headless " << Stage << " validation: " << LitPixelCount << " lit pixels\n";
}

void Renderer::ValidateHeadlessPresentation(core::Window &Window)
{
	if (!this->HeadlessPresentationValidation || !this->HeadlessValidationFrameEligible || this->PresentationValidated)
		return;
	const core::WindowExtent Extent = Window.GetFramebufferExtent();
	const uint32 Width = Extent.Width;
	const uint32 Height = Extent.Height;
	if (Width == 0 || Height == 0)
		throw std::runtime_error("Headless presentation validation requires a non-zero window extent");
	const uint64 ByteCount = static_cast<uint64>(Width) * static_cast<uint64>(Height) * 4U;
	if (ByteCount > static_cast<uint64>(std::numeric_limits<usize>::max()))
		throw std::runtime_error("Headless presentation validation readback exceeds addressable memory");

	std::vector<uint8> Pixels(static_cast<usize>(ByteCount));
	// The renderer owns these presentation-readback states. Restore the engine
	// defaults explicitly instead of querying mutable driver state.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, static_cast<GLsizei>(Width), static_cast<GLsizei>(Height), GL_RGBA, GL_UNSIGNED_BYTE, Pixels.data());
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadBuffer(GL_BACK);
	this->Device->CheckErrors("Headless presentation validation");

	uint64 LitPixelCount = 0;
	for (uint64 Offset = 0; Offset < ByteCount; Offset += 4U)
	{
		if (static_cast<uint32>(Pixels[static_cast<usize>(Offset)]) + static_cast<uint32>(Pixels[static_cast<usize>(Offset + 1U)]) +
				static_cast<uint32>(Pixels[static_cast<usize>(Offset + 2U)]) >
			3U)
			++LitPixelCount;
	}
	if (LitPixelCount == 0)
		throw std::runtime_error("Headless presentation validation found an all-black presented frame");
	if (!this->HeadlessPresentationCapturePath.empty())
	{
		const uint64 RowBytes = static_cast<uint64>(Width) * 3U;
		const uint64 PaddedRowBytes = (RowBytes + 3U) & ~uint64{3U};
		const uint64 ImageBytes = PaddedRowBytes * Height;
		constexpr uint32 HeaderBytes = 54U;
		if (ImageBytes > static_cast<uint64>(std::numeric_limits<uint32>::max()) - HeaderBytes)
			throw std::runtime_error("Headless presentation capture exceeds the BMP size limit");
		const std::filesystem::path Parent = this->HeadlessPresentationCapturePath.parent_path();
		if (!Parent.empty())
			std::filesystem::create_directories(Parent);
		std::ofstream Capture(this->HeadlessPresentationCapturePath, std::ios::binary | std::ios::trunc);
		if (!Capture)
			throw std::runtime_error("Could not create the headless presentation capture");
		const auto WriteUInt16 = [&Capture](const uint16 Value)
		{
			const std::array<uint8, 2> Bytes{static_cast<uint8>(Value), static_cast<uint8>(Value >> 8U)};
			Capture.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
		};
		const auto WriteUInt32 = [&Capture](const uint32 Value)
		{
			const std::array<uint8, 4> Bytes{static_cast<uint8>(Value), static_cast<uint8>(Value >> 8U), static_cast<uint8>(Value >> 16U),
											 static_cast<uint8>(Value >> 24U)};
			Capture.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
		};
		Capture.put('B');
		Capture.put('M');
		WriteUInt32(HeaderBytes + static_cast<uint32>(ImageBytes));
		WriteUInt16(0);
		WriteUInt16(0);
		WriteUInt32(HeaderBytes);
		WriteUInt32(40U);
		WriteUInt32(Width);
		WriteUInt32(Height);
		WriteUInt16(1U);
		WriteUInt16(24U);
		WriteUInt32(0U);
		WriteUInt32(static_cast<uint32>(ImageBytes));
		WriteUInt32(2'835U);
		WriteUInt32(2'835U);
		WriteUInt32(0U);
		WriteUInt32(0U);
		const std::array<uint8, 3> Padding{};
		for (uint32 Row = 0; Row < Height; ++Row)
		{
			for (uint32 Column = 0; Column < Width; ++Column)
			{
				const usize Offset = (static_cast<usize>(Row) * Width + Column) * 4U;
				const std::array<uint8, 3> BGR{Pixels[Offset + 2U], Pixels[Offset + 1U], Pixels[Offset]};
				Capture.write(reinterpret_cast<const char *>(BGR.data()), BGR.size());
			}
			Capture.write(reinterpret_cast<const char *>(Padding.data()), static_cast<std::streamsize>(PaddedRowBytes - RowBytes));
		}
		Capture.flush();
		if (!Capture)
			throw std::runtime_error("Could not write the headless presentation capture");
	}
	std::cerr << "Headless presentation validation: " << LitPixelCount << " non-black pixels\n";
	this->PresentationValidated = true;
}

} // namespace pipeline::render
