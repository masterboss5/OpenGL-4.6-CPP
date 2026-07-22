#include "OpenGLRenderer.h"

#include "src/core/window/Window.h"
#include "src/pipeline/device/Device.h"
#include "src/renderer/SceneExtractor.h"
#include "src/resource/asset/AssetManager.h"
#include "src/scene/Camera.h"
#include "src/scene/Scene.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace
{
constexpr uint32 MaximumRenderItems = 65'536;
constexpr uint32 MaximumSkinMatrices = 262'144;
constexpr uint32 MaximumMorphWeights = 262'144;
constexpr uint32 MaximumLights = 100;
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
	for (const renderer::RenderItem &Item : Collection.GetRenderItems())
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
	Hash = HashBytes(Hash, SkinMatrices.data(), SkinMatrices.size() * sizeof(renderer::GPUSkinMatrixRecord));
	const auto &MorphWeights = Collection.GetMorphWeights();
	Hash = HashBytes(Hash, MorphWeights.data(), MorphWeights.size() * sizeof(renderer::GPUMorphWeightRecord));
	return Hash;
}
[[nodiscard]] GLenum ToOpenGlIndexFormat(const renderer::RenderIndexFormat Format) noexcept
{
	return Format == renderer::RenderIndexFormat::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
}
[[nodiscard]] uint32 IndexElementSize(const renderer::RenderIndexFormat Format) noexcept
{
	return Format == renderer::RenderIndexFormat::UInt16 ? sizeof(uint16) : sizeof(uint32);
}

void CopyToMappedBuffer(const renderer::FrameBufferSlice &Destination, const void *Source, const uint64 SizeInBytes,
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

OpenGLRenderer::OpenGLRenderer(pipeline::device::Device &Device, const bool HeadlessPresentationValidation)
	: Device(&Device), HeadlessPresentationValidation(HeadlessPresentationValidation), MeshGPUCache(Device),
	  LightBufferManager(MaximumLights), RenderGraph(Device)
{
	(void)this->Device->RequireCurrentContext();
	glCreateFramebuffers(1, &ShadowFramebuffer);
	this->FrameResources = std::make_unique<renderer::FrameResourceRing>(
		Device,
		renderer::FrameResourceCapacitySpecification{.FrameConstants = sizeof(renderer::GPUFrameConstants),
													 .Materials = sizeof(renderer::GPUMaterialRecord) * MaximumRenderItems,
													 .ShadowData = sizeof(renderer::GPUShadowRecord) * renderer::MaximumShadowRecordCount,
													 .Lights = sizeof(renderer::GPULightRecord) * MaximumLights,
													 .CandidateInstances = sizeof(renderer::PreparedInstance) * MaximumRenderItems,
													 .ShadowInstances = sizeof(renderer::PreparedInstance) * MaximumRenderItems,
													 .VisibleInstances = sizeof(renderer::PreparedInstance) * MaximumRenderItems,
													 .IndirectCommands = sizeof(renderer::RenderCommand) * MaximumRenderItems,
													 .BatchMetadata = sizeof(renderer::RenderBatch) * MaximumRenderItems,
													 .VisibilityScratch = sizeof(uint32) * (MaximumRenderItems * 4U + 512U),
													 .SkinMatrices = sizeof(renderer::GPUSkinMatrixRecord) * MaximumSkinMatrices,
													 .MorphWeights = sizeof(renderer::GPUMorphWeightRecord) * MaximumMorphWeights});
	this->Device->CheckErrors("OpenGLRenderer creation");
}

OpenGLRenderer::~OpenGLRenderer()
{
	this->FrameResources.reset();
	if (ShadowFramebuffer != 0 && this->Device != nullptr && this->Device->CanIssueCommands())
		glDeleteFramebuffers(1, &ShadowFramebuffer);
	ShadowFramebuffer = 0;
}

uint32 OpenGLRenderer::GetDrawCount() const noexcept
{
	return DrawCount;
}
uint32 OpenGLRenderer::GetObjectsDrawn() const noexcept
{
	return ObjectsDrawn;
}

void OpenGLRenderer::SetBackgroundColor(const glm::vec3 &Color)
{
	if (!std::isfinite(Color.x) || !std::isfinite(Color.y) || !std::isfinite(Color.z) || glm::any(glm::lessThan(Color, glm::vec3(0.0f))))
	{
		throw std::invalid_argument("Renderer background color must contain finite non-negative linear values");
	}
	this->BackgroundColor = Color;
}

const glm::vec3 &OpenGLRenderer::GetBackgroundColor() const noexcept
{
	return this->BackgroundColor;
}

void OpenGLRenderer::Render(const world::Scene &Scene, resource::AssetManager &Assets, const Camera &Camera)
{
	if (CollectingFrame)
		throw std::logic_error("OpenGLRenderer accepts one Scene extraction per frame");
	Assets.RealizeAllPendingGPU(*this->Device);
	SceneCollection.BeginFrame(FrameNumber);
	this->CurrentRenderTransforms.clear();
	renderer::SceneExtractor Extractor(*this->Device, this->MeshGPUCache, Assets, this->PreviousRenderTransforms,
									   this->CurrentRenderTransforms);
	Extractor.Extract(Scene, Camera, SceneCollection);
	this->PreviousRenderTransforms.swap(this->CurrentRenderTransforms);
	this->ObjectsDrawn = Scene.GetObjectCount();
	CollectingFrame = true;
}

void OpenGLRenderer::UploadFrameConstants(const Camera &Camera, const core::WindowExtent Extent, renderer::FrameResources &FrameResources)
{
	const glm::mat4 Projection = Camera.GetProjectionMatrix(Extent);
	const glm::mat4 View = Camera.GetViewMatrix();
	const glm::mat4 ViewProjection = Projection * View;
	const renderer::GPUFrameConstants Frame{
		.Projection = Projection,
		.View = View,
		.ViewProjection = ViewProjection,
		.PreviousViewProjection = PreviousViewProjection,
		.InverseViewProjection = glm::inverse(ViewProjection),
		.CameraPositionAndNear = glm::vec4(Camera.Position, Camera.NearPlane),
		.RenderExtentAndFar = glm::vec4(static_cast<float32>(Extent.Width), static_cast<float32>(Extent.Height), Camera.FarPlane, 0.0f),
		.CountsAndFrame = glm::uvec4(0, 0, 0, static_cast<uint32>(FrameNumber)),
		.BackgroundColor = glm::vec4(this->BackgroundColor, 1.0f)};
	CopyToMappedBuffer(FrameResources.FrameConstants, &Frame, sizeof(Frame), "frame constants");
	glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(renderer::RendererBinding::FrameConstants),
					 FrameResources.FrameConstants.Buffer);
	PreviousViewProjection = Frame.ViewProjection;
}

void OpenGLRenderer::RenderScene(const pipeline::shader::GraphicsPipeline &Pipeline, const Camera &Camera, core::Window &Window)
{
	if (!CollectingFrame)
	{
		return;
	}
	auto Recovery = MakeScopeExit([this]() noexcept { this->RecoverFailedFrame(); });
	const core::WindowExtent Extent = Window.GetFramebufferExtent();
	if (!Extent.IsValid())
		return;
	SceneCollection.Seal();
	this->DrawCount = 0;
	const renderer::RenderPreparationResult Prepared =
		ScenePreparation.Prepare(SceneCollection, Camera.GetProjectionMatrix(Extent) * Camera.GetViewMatrix(), 0, 0);
	if (Prepared.CandidateInstances.size() > MaximumRenderItems || Prepared.CandidateCommands.size() > MaximumRenderItems)
	{
		throw std::runtime_error("Renderer frame capacity exceeded; increase MaximumRenderItems before submitting more geometry");
	}

	renderer::FrameResources &Frame = FrameResources->Acquire(FrameNumber);
	this->UploadFrameConstants(Camera, Extent, Frame);
	const std::span<const renderer::GPULightRecord> LightRecords = this->LightBufferManager.GetGPURecords();
	CopyToMappedBuffer(Frame.Lights, LightRecords.data(), LightRecords.size_bytes(), "lights");
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Lights), Frame.Lights.Buffer);
	this->MeshGPUCache.Collect(FrameNumber, renderer::FrameResourceRing::FrameCount);
	if (SceneCollection.GetSkinningMatrices().size() > MaximumSkinMatrices)
		throw std::runtime_error("Renderer skinning palette capacity exceeded");
	CopyToMappedBuffer(Frame.SkinMatrices, SceneCollection.GetSkinningMatrices().data(),
					   SceneCollection.GetSkinningMatrices().size() * sizeof(renderer::GPUSkinMatrixRecord), "skin matrices");
	if (SceneCollection.GetMorphWeights().size() > MaximumMorphWeights)
		throw std::runtime_error("Renderer morph weight capacity exceeded");
	CopyToMappedBuffer(Frame.MorphWeights, SceneCollection.GetMorphWeights().data(),
					   SceneCollection.GetMorphWeights().size() * sizeof(renderer::GPUMorphWeightRecord), "morph weights");
	if (!Prepared.CandidateInstances.empty())
	{
		const uint64 InstanceBytes = static_cast<uint64>(Prepared.CandidateInstances.size() * sizeof(renderer::PreparedInstance));
		CopyToMappedBuffer(Frame.CandidateInstances, Prepared.CandidateInstances.data(), InstanceBytes, "candidate instances");
		CopyToMappedBuffer(Frame.VisibleInstances, Prepared.CandidateInstances.data(), InstanceBytes, "visible instances");
	}
	CopyToMappedBuffer(Frame.IndirectCommands, Prepared.CandidateCommands.data(),
					   Prepared.CandidateCommands.size() * sizeof(renderer::RenderCommand), "indirect commands");
	CopyToMappedBuffer(Frame.BatchMetadata, Prepared.Batches.data(), Prepared.Batches.size() * sizeof(renderer::RenderBatch),
					   "batch metadata");
	if (Prepared.Materials.size() > MaximumRenderItems)
		throw std::runtime_error("Renderer material capacity exceeded; increase MaximumRenderItems before submitting more materials");
	CopyToMappedBuffer(Frame.Materials, Prepared.Materials.data(), Prepared.Materials.size() * sizeof(renderer::GPUMaterialRecord),
					   "materials");
	glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, Frame.VisibleInstances.Buffer);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, Frame.IndirectCommands.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Materials), Frame.Materials.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::SkinMatrices), Frame.SkinMatrices.Buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::MorphWeights), Frame.MorphWeights.Buffer);

	Pipeline.Bind();
	for (uint32 BatchIndex = 0; BatchIndex < Prepared.Batches.size(); ++BatchIndex)
	{
		const renderer::RenderBatch &Batch = Prepared.Batches[BatchIndex];
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::MorphDeltas), Batch.MorphDeltaBuffer);
		glBindVertexArray(Batch.VertexArray);
		const uintptr_t Offset = static_cast<uintptr_t>(BatchIndex) * sizeof(renderer::RenderCommand);
		glMultiDrawElementsIndirect(GL_TRIANGLES, ToOpenGlIndexFormat(Batch.IndexFormat), reinterpret_cast<const void *>(Offset), 1,
									sizeof(renderer::RenderCommand));
		++DrawCount;
	}

	FrameResources->Retire(SceneCollection.ReleaseAssetPins());
	++FrameNumber;
	CollectingFrame = false;
	SceneCollection.Clear();
	ObjectsDrawn = 0;
	Recovery.Release();
}

void OpenGLRenderer::RenderScene(const renderer::RenderPassPipelineSet &Pipelines, const Camera &Camera, core::Window &Window)
{
	if (!CollectingFrame)
		return;
	auto Recovery = MakeScopeExit([this]() noexcept { this->RecoverFailedFrame(); });
	const core::WindowExtent WindowExtent = Window.GetFramebufferExtent();
	if (!WindowExtent.IsValid())
		return;
	const bool CameraCut = HasPreviousCameraState && (glm::distance(Camera.Position, PreviousCameraPosition) > 2.0f ||
													  glm::dot(glm::normalize(Camera.Front), glm::normalize(PreviousCameraFront)) < 0.95f);
	if (CameraCut)
	{
		HierarchicalDepthHistoryValid = false;
		TemporalHistoryValid = false;
	}
	SceneCollection.Seal();
	this->DrawCount = 0;
	const renderer::RenderPreparationResult Prepared =
		ScenePreparation.Prepare(SceneCollection, Camera.GetProjectionMatrix(WindowExtent) * Camera.GetViewMatrix(), 0, 0);
	const renderer::RenderPreparationResult ShadowPrepared = ScenePreparation.Prepare(SceneCollection, glm::mat4(1.0f), 0, 0, false, true);
	const uint64 ShadowCasterSignature = CalculateShadowCasterSignature(SceneCollection);
	if (Prepared.CandidateInstances.size() > MaximumRenderItems || Prepared.CandidateCommands.size() > MaximumRenderItems ||
		ShadowPrepared.CandidateInstances.size() > MaximumRenderItems || ShadowPrepared.CandidateCommands.size() > MaximumRenderItems)
		throw std::runtime_error("Renderer frame capacity exceeded; increase MaximumRenderItems before submitting more geometry");

	renderer::FrameResources &Frame = FrameResources->Acquire(FrameNumber);
	this->UploadFrameConstants(Camera, WindowExtent, Frame);
	const std::span<const renderer::GPULightRecord> LightRecords = this->LightBufferManager.GetGPURecords();
	CopyToMappedBuffer(Frame.Lights, LightRecords.data(), LightRecords.size_bytes(), "lights");
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Lights), Frame.Lights.Buffer);
	this->MeshGPUCache.Collect(FrameNumber, renderer::FrameResourceRing::FrameCount);
	if (SceneCollection.GetSkinningMatrices().size() > MaximumSkinMatrices)
		throw std::runtime_error("Renderer skinning palette capacity exceeded");
	CopyToMappedBuffer(Frame.SkinMatrices, SceneCollection.GetSkinningMatrices().data(),
					   SceneCollection.GetSkinningMatrices().size() * sizeof(renderer::GPUSkinMatrixRecord), "skin matrices");
	if (SceneCollection.GetMorphWeights().size() > MaximumMorphWeights)
		throw std::runtime_error("Renderer morph weight capacity exceeded");
	CopyToMappedBuffer(Frame.MorphWeights, SceneCollection.GetMorphWeights().data(),
					   SceneCollection.GetMorphWeights().size() * sizeof(renderer::GPUMorphWeightRecord), "morph weights");
	const uint64 InstanceBytes = static_cast<uint64>(Prepared.CandidateInstances.size() * sizeof(renderer::PreparedInstance));
	if (InstanceBytes != 0)
	{
		CopyToMappedBuffer(Frame.CandidateInstances, Prepared.CandidateInstances.data(), InstanceBytes, "candidate instances");
		CopyToMappedBuffer(Frame.VisibleInstances, Prepared.CandidateInstances.data(), InstanceBytes, "visible instances");
	}
	const uint64 ShadowInstanceBytes = static_cast<uint64>(ShadowPrepared.CandidateInstances.size() * sizeof(renderer::PreparedInstance));
	if (ShadowInstanceBytes != 0)
		CopyToMappedBuffer(Frame.ShadowInstances, ShadowPrepared.CandidateInstances.data(), ShadowInstanceBytes, "shadow instances");
	CopyToMappedBuffer(Frame.IndirectCommands, Prepared.CandidateCommands.data(),
					   Prepared.CandidateCommands.size() * sizeof(renderer::RenderCommand), "indirect commands");
	CopyToMappedBuffer(Frame.BatchMetadata, Prepared.Batches.data(), Prepared.Batches.size() * sizeof(renderer::RenderBatch),
					   "batch metadata");
	// Visibility compaction writes the final per-batch instance counts directly
	// into DrawElementsIndirectCommand. Each command starts empty every frame.
	for (uint32 CommandIndex = 0; CommandIndex < Prepared.CandidateCommands.size(); ++CommandIndex)
	{
		static_cast<renderer::RenderCommand *>(Frame.IndirectCommands.MappedMemory)[CommandIndex].InstanceCount = 0;
	}
	if (Prepared.Materials.size() > MaximumRenderItems)
		throw std::runtime_error("Renderer material capacity exceeded; increase MaximumRenderItems before submitting more materials");
	CopyToMappedBuffer(Frame.Materials, Prepared.Materials.data(), Prepared.Materials.size() * sizeof(renderer::GPUMaterialRecord),
					   "materials");
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Materials), Frame.Materials.Buffer);
	glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

	std::vector<renderer::GPUShadowRecord> ShadowRecords(renderer::MaximumShadowRecordCount);
	uint32 DirectionalCascadeCount = 0;
	uint32 SpotShadowCount = 0;
	uint32 PointShadowFaceCount = 0;
	const std::vector<DirectionalLightSource> &DirectionalLights = LightBufferManager.GetDirectionalLights();
	if (!DirectionalLights.empty())
	{
		constexpr std::array<float32, renderer::DirectionalShadowCascadeCount> CascadeRadii{25.0f, 75.0f, 200.0f, 500.0f};
		const glm::vec3 Direction = glm::normalize(DirectionalLights.front().Direction);
		for (uint32 Cascade = 0; Cascade < renderer::DirectionalShadowCascadeCount; ++Cascade)
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
		DirectionalCascadeCount = renderer::DirectionalShadowCascadeCount;
	}
	const std::vector<SpotLightSource> &SpotLights = LightBufferManager.GetSpotLights();
	SpotShadowCount = std::min(static_cast<uint32>(SpotLights.size()), renderer::MaximumSpotShadowCount);
	for (uint32 SpotIndex = 0; SpotIndex < SpotShadowCount; ++SpotIndex)
	{
		const SpotLightSource &Light = SpotLights[SpotIndex];
		const glm::vec3 Direction = glm::normalize(Light.Direction);
		const glm::vec3 Up = glm::abs(Direction.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		const float32 Range = 1000.0f;
		ShadowRecords[renderer::DirectionalShadowCascadeCount + SpotIndex] = {
			.ViewProjection = glm::perspectiveRH_ZO(glm::acos(glm::clamp(Light.OuterCutOff, -1.0f, 1.0f)) * 2.0f, 1.0f, 0.1f, Range) *
							  glm::lookAt(Light.Position, Light.Position + Direction, Up),
			.AtlasScaleBias = glm::vec4(1.0f, 1.0f, static_cast<float32>(SpotIndex), 0.0f),
			.DepthBiasAndFilter = glm::vec4(0.001f, 0.003f, Range, 0.0f)};
	}
	const std::vector<PointLightSource> &PointLights = LightBufferManager.GetPointLights();
	const uint32 PointShadowCount = std::min(static_cast<uint32>(PointLights.size()), renderer::MaximumPointShadowCount);
	constexpr std::array<glm::vec3, 6> PointFaceDirections{glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
														   glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
														   glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
	constexpr std::array<glm::vec3, 6> PointFaceUps{glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
													glm::vec3(0.0f, 0.0f, 1.0f),  glm::vec3(0.0f, 0.0f, -1.0f),
													glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)};
	for (uint32 PointIndex = 0; PointIndex < PointShadowCount; ++PointIndex)
	{
		constexpr float32 Range = 1000.0f;
		for (uint32 Face = 0; Face < 6; ++Face)
		{
			const uint32 RecordIndex = renderer::DirectionalShadowCascadeCount + renderer::MaximumSpotShadowCount + PointIndex * 6U + Face;
			ShadowRecords[RecordIndex] = {.ViewProjection =
											  glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, Range) *
											  glm::lookAt(PointLights[PointIndex].Position,
														  PointLights[PointIndex].Position + PointFaceDirections[Face], PointFaceUps[Face]),
										  .AtlasScaleBias = glm::vec4(1.0f, 1.0f, static_cast<float32>(PointIndex * 6U + Face), 0.0f),
										  .DepthBiasAndFilter = glm::vec4(0.002f, 0.004f, Range, 0.0f)};
		}
	}
	PointShadowFaceCount = PointShadowCount * 6U;
	CopyToMappedBuffer(Frame.ShadowData, ShadowRecords.data(), ShadowRecords.size() * sizeof(renderer::GPUShadowRecord), "shadow data");

	const renderer::graph::Extent2D Extent{WindowExtent.Width, WindowExtent.Height};
	RenderGraph.BeginFrame(Extent);
	auto Import = [this](string Name, const renderer::FrameBufferSlice &Slice)
	{
		return RenderGraph.ImportBuffer(
			{.DebugName = std::move(Name), .SizeInBytes = Slice.CapacityInBytes, .StorageFlags = GL_DYNAMIC_STORAGE_BIT}, Slice.Buffer);
	};
	const renderer::HybridDeferredFrameInputs Inputs{.Extent = Extent,
													 .CandidateInstances = Import("CandidateInstances", Frame.CandidateInstances),
													 .ShadowInstances = Import("ShadowInstances", Frame.ShadowInstances),
													 .VisibleInstances = Import("VisibleInstances", Frame.VisibleInstances),
													 .IndirectCommands = Import("IndirectCommands", Frame.IndirectCommands),
													 .BatchMetadata = Import("BatchMetadata", Frame.BatchMetadata),
													 .VisibilityScratch = Import("VisibilityScratch", Frame.VisibilityScratch),
													 .TemporalHistoryWriteIndex = static_cast<uint32>(FrameNumber & 1U)};

	auto Dispatch = [](const pipeline::shader::ComputePipeline &Pipeline, renderer::graph::RenderGraphContext &Context,
					   renderer::graph::TextureHandle Output)
	{
		const renderer::graph::Extent2D Size = Context.GetExtent(Output);
		Pipeline.Bind();
		glDispatchCompute((Size.Width + 7U) / 8U, (Size.Height + 7U) / 8U, 1);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT |
						GL_TEXTURE_FETCH_BARRIER_BIT);
	};
	auto DrawBatches = [this, &Prepared, &Frame](renderer::graph::RenderGraphContext &Context,
												 const pipeline::shader::GraphicsPipeline &Pipeline, renderer::RenderPassClass RequiredPass)
	{
		Context.ValidateGraphicsPipelineTargets(Pipeline);
		Pipeline.Bind();
		glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(renderer::RendererBinding::FrameConstants), Frame.FrameConstants.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, Frame.VisibleInstances.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Materials), Frame.Materials.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::SkinMatrices), Frame.SkinMatrices.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::MorphWeights), Frame.MorphWeights.Buffer);
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, Frame.IndirectCommands.Buffer);
		bool TwoSidedState = false;
		for (uint32 BatchIndex = 0; BatchIndex < Prepared.Batches.size(); ++BatchIndex)
		{
			const renderer::RenderBatch &Batch = Prepared.Batches[BatchIndex];
			if (Batch.PassClass != RequiredPass)
				continue;
			if (Batch.VertexDescriptor == nullptr)
				throw std::logic_error("RenderBatch is missing its VertexDescriptor");
			Pipeline.ValidateVertexDescriptor(*Batch.VertexDescriptor);
			if (Batch.TwoSided != TwoSidedState)
			{
				if (Batch.TwoSided)
					glDisable(GL_CULL_FACE);
				else if (Pipeline.GetDescriptor().State.Rasterizer.CullMode != pipeline::shader::CullMode::None)
					glEnable(GL_CULL_FACE);
				TwoSidedState = Batch.TwoSided;
			}
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::MorphDeltas), Batch.MorphDeltaBuffer);
			glBindVertexArray(Batch.VertexArray);
			const uintptr_t Offset = static_cast<uintptr_t>(BatchIndex) * sizeof(renderer::RenderCommand);
			glMultiDrawElementsIndirect(Pipeline.GetGLTopology(), ToOpenGlIndexFormat(Batch.IndexFormat),
										reinterpret_cast<const void *>(Offset), 1, sizeof(renderer::RenderCommand));
			++DrawCount;
		}
	};

	const auto RenderShadowLayers = [this, &Pipelines, &Frame, &ShadowPrepared, &ShadowRecords,
									 ShadowCasterSignature](GLuint Texture, uint32 FirstLayer, uint32 LayerCount, uint32 FirstRecord,
															renderer::graph::Extent2D ShadowExtent)
	{
		if (LayerCount == 0)
			return;
		Pipelines.ShadowDepth.Bind();
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Instances), Frame.ShadowInstances.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Materials), Frame.Materials.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ShadowData), Frame.ShadowData.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::SkinMatrices), Frame.SkinMatrices.Buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::MorphWeights), Frame.MorphWeights.Buffer);
		glNamedFramebufferDrawBuffer(ShadowFramebuffer, GL_NONE);
		glNamedFramebufferReadBuffer(ShadowFramebuffer, GL_NONE);
		bool TwoSidedState = false;
		for (uint32 Layer = 0; Layer < LayerCount; ++Layer)
		{
			const renderer::GPUShadowRecord &ShadowView = ShadowRecords.at(FirstRecord + Layer);
			uint64 ShadowSignature = HashMatrix(HashValue(ShadowCasterSignature, static_cast<uint32>(Texture)), ShadowView.ViewProjection);
			ShadowSignature = HashValue(ShadowSignature, FirstLayer + Layer);
			const uint32 CacheKey = FirstRecord + Layer;
			ShadowLayerCacheEntry &CachedLayer = this->ShadowLayerCache[CacheKey];
			if (CachedLayer.Valid && CachedLayer.Texture == Texture && CachedLayer.Layer == FirstLayer + Layer &&
				CachedLayer.Signature == ShadowSignature)
				continue;
			glNamedFramebufferTextureLayer(ShadowFramebuffer, GL_DEPTH_ATTACHMENT, Texture, 0, static_cast<GLint>(FirstLayer + Layer));
			if (glCheckNamedFramebufferStatus(ShadowFramebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				throw std::runtime_error("Shadow framebuffer is incomplete");
			glBindFramebuffer(GL_FRAMEBUFFER, ShadowFramebuffer);
			glViewport(0, 0, static_cast<GLsizei>(ShadowExtent.Width), static_cast<GLsizei>(ShadowExtent.Height));
			glClearDepth(1.0);
			glClear(GL_DEPTH_BUFFER_BIT);
			Pipelines.ShadowDepth.SetVertexUniformUInt("shadowViewIndex", FirstRecord + Layer);
			for (const renderer::RenderBatch &Batch : ShadowPrepared.Batches)
			{
				if (Batch.PassClass == renderer::RenderPassClass::Transparency)
					continue;
				if (Batch.VertexDescriptor == nullptr)
					throw std::logic_error("RenderBatch is missing its VertexDescriptor");
				Pipelines.ShadowDepth.ValidateVertexDescriptor(*Batch.VertexDescriptor);
				if (Batch.TwoSided != TwoSidedState)
				{
					if (Batch.TwoSided)
						glDisable(GL_CULL_FACE);
					else if (Pipelines.ShadowDepth.GetDescriptor().State.Rasterizer.CullMode != pipeline::shader::CullMode::None)
						glEnable(GL_CULL_FACE);
					TwoSidedState = Batch.TwoSided;
				}
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::MorphDeltas),
								 Batch.MorphDeltaBuffer);
				glBindVertexArray(Batch.VertexArray);
				glDrawElementsInstancedBaseVertexBaseInstance(
					Pipelines.ShadowDepth.GetGLTopology(), static_cast<GLsizei>(Batch.IndexCount), ToOpenGlIndexFormat(Batch.IndexFormat),
					reinterpret_cast<const void *>(static_cast<uintptr_t>(Batch.FirstIndex) * IndexElementSize(Batch.IndexFormat)),
					static_cast<GLsizei>(Batch.CandidateCount), Batch.BaseVertex, Batch.FirstCandidate);
			}
			CachedLayer = {.Texture = Texture, .Layer = FirstLayer + Layer, .Signature = ShadowSignature, .Valid = true};
		}
	};

	const renderer::HybridDeferredPassCallbacks Callbacks{
		.DirectionalShadows =
			[&RenderShadowLayers, DirectionalCascadeCount](renderer::graph::RenderGraphContext &Context,
														   const renderer::HybridDeferredFrameResources &Resources)
		{
			RenderShadowLayers(Context.GetTexture(Resources.DirectionalShadowAtlas), 0, DirectionalCascadeCount, 0,
							   Context.GetExtent(Resources.DirectionalShadowAtlas));
		},
		.SpotShadows =
			[&RenderShadowLayers, SpotShadowCount](renderer::graph::RenderGraphContext &Context,
												   const renderer::HybridDeferredFrameResources &Resources)
		{
			RenderShadowLayers(Context.GetTexture(Resources.SpotShadowAtlas), 0, SpotShadowCount, renderer::DirectionalShadowCascadeCount,
							   Context.GetExtent(Resources.SpotShadowAtlas));
		},
		.PointShadows =
			[&RenderShadowLayers, PointShadowFaceCount](renderer::graph::RenderGraphContext &Context,
														const renderer::HybridDeferredFrameResources &Resources)
		{
			RenderShadowLayers(Context.GetTexture(Resources.PointShadowArray), 0, PointShadowFaceCount,
							   renderer::DirectionalShadowCascadeCount + renderer::MaximumSpotShadowCount,
							   Context.GetExtent(Resources.PointShadowArray));
		},
		.MainVisibility =
			[this, &Pipelines, &Frame, &Prepared](renderer::graph::RenderGraphContext &Context,
												  const renderer::HybridDeferredFrameResources &Resources)
		{
			const uint32 Zero = 0;
			const uint32 CandidateCount = static_cast<uint32>(Prepared.CandidateInstances.size());
			const uint32 BatchCount = static_cast<uint32>(Prepared.Batches.size());
			const uint32 PyramidMipCount = static_cast<uint32>(std::bit_width(
				std::max(Context.GetExtent(Resources.HierarchicalDepth).Width, Context.GetExtent(Resources.HierarchicalDepth).Height)));
			const bool HistoryMatchesExtent =
				HierarchicalDepthHistoryValid &&
				HierarchicalDepthHistoryExtent.Width == Context.GetExtent(Resources.HierarchicalDepth).Width &&
				HierarchicalDepthHistoryExtent.Height == Context.GetExtent(Resources.HierarchicalDepth).Height;
			glClearNamedBufferData(Frame.VisibilityScratch.Buffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &Zero);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Candidates),
							 Frame.CandidateInstances.Buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Instances),
							 Frame.VisibleInstances.Buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::VisibilityScratch),
							 Frame.VisibilityScratch.Buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::IndirectCommands),
							 Frame.IndirectCommands.Buffer);
			glBindTextureUnit(0, Context.GetTexture(Resources.HierarchicalDepth));
			if (CandidateCount == 0 || BatchCount == 0)
				return;
			Pipelines.VisibilityCull.SetUniformUInt("candidateCount", CandidateCount);
			Pipelines.VisibilityCull.SetUniformUInt("pyramidMipCount", PyramidMipCount);
			Pipelines.VisibilityCull.SetUniformUInt("historyValid", HistoryMatchesExtent ? 1U : 0U);
			Pipelines.VisibilityCull.SetUniformUInt("scratchCapacity", MaximumRenderItems);
			Pipelines.VisibilityCull.Bind();
			glDispatchCompute((CandidateCount + 63U) / 64U, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
			const uint32 VisibilityScanBlockCount = (BatchCount + 255U) / 256U;
			Pipelines.VisibilityPrefixScan.SetUniformUInt("batchCount", BatchCount);
			Pipelines.VisibilityPrefixScan.SetUniformUInt("scratchCapacity", MaximumRenderItems);
			Pipelines.VisibilityPrefixScan.Bind();
			glDispatchCompute(VisibilityScanBlockCount, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
			Pipelines.VisibilityBlockPrefixScan.SetUniformUInt("blockCount", VisibilityScanBlockCount);
			Pipelines.VisibilityBlockPrefixScan.SetUniformUInt("scratchCapacity", MaximumRenderItems);
			Pipelines.VisibilityBlockPrefixScan.Bind();
			glDispatchCompute(1, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
			Pipelines.VisibilityFinalize.SetUniformUInt("batchCount", BatchCount);
			Pipelines.VisibilityFinalize.SetUniformUInt("scratchCapacity", MaximumRenderItems);
			Pipelines.VisibilityFinalize.Bind();
			glDispatchCompute(VisibilityScanBlockCount, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
			Pipelines.VisibilityScatter.SetUniformUInt("candidateCount", CandidateCount);
			Pipelines.VisibilityScatter.SetUniformUInt("scratchCapacity", MaximumRenderItems);
			Pipelines.VisibilityScatter.Bind();
			glDispatchCompute((CandidateCount + 63U) / 64U, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		},
		.DepthPrepass =
			[&Pipelines, &DrawBatches](renderer::graph::RenderGraphContext &Context, const renderer::HybridDeferredFrameResources &)
		{ DrawBatches(Context, Pipelines.DepthPrepass, renderer::RenderPassClass::GBuffer); },
		.HierarchicalDepth =
			[&Pipelines, Extent](renderer::graph::RenderGraphContext &Context, const renderer::HybridDeferredFrameResources &Resources)
		{
			const GLuint DepthTexture = Context.GetTexture(Resources.Depth);
			const GLuint PyramidTexture = Context.GetTexture(Resources.HierarchicalDepth);
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
				Pipelines.HierarchicalDepth.SetUniformUInt2("sourceExtent", SourceWidth, SourceHeight);
				Pipelines.HierarchicalDepth.SetUniformUInt("sourceMip", Mip == 0 ? 0U : Mip - 1U);
				Pipelines.HierarchicalDepth.SetUniformUInt("sourceScale", Mip == 0 ? 1U : 2U);
				glDispatchCompute((OutputWidth + 7U) / 8U, (OutputHeight + 7U) / 8U, 1);
				glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
			}
		},
		.GBuffer =
			[this, &Pipelines, &DrawBatches](renderer::graph::RenderGraphContext &Context,
											 const renderer::HybridDeferredFrameResources &Resources)
		{
			DrawBatches(Context, Pipelines.GBuffer, renderer::RenderPassClass::GBuffer);
			if (this->HeadlessPresentationValidation && !this->PresentationValidated)
			{
				this->ValidateHeadlessDepthCoverage(Context.GetTexture(Resources.Depth), Context.GetExtent(Resources.Depth));
				this->ValidateHeadlessColorCoverage(Context.GetTexture(Resources.GBufferBaseColor),
													Context.GetExtent(Resources.GBufferBaseColor), "G-buffer base color");
				this->ValidateHeadlessColorCoverage(Context.GetTexture(Resources.GBufferNormalRoughness),
													Context.GetExtent(Resources.GBufferNormalRoughness), "G-buffer normal");
			}
		},
		.ClusteredLights =
			[this, &Pipelines, &Frame](renderer::graph::RenderGraphContext &Context,
									   const renderer::HybridDeferredFrameResources &Resources)
		{
			constexpr uint32 ClusterCount = 32U * 18U * 24U;
			glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(renderer::RendererBinding::FrameConstants),
							 Frame.FrameConstants.Buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Lights), Frame.Lights.Buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ClusterHeaders),
							 Context.GetBuffer(Resources.ClusterHeaders));
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ClusterIndices),
							 Context.GetBuffer(Resources.ClusterIndices));
			Pipelines.ClusteredLights.SetUniformUInt("lightCount",
													 static_cast<uint32>(this->LightBufferManager.GetTotalLightSourceCount()));
			Pipelines.ClusteredLights.SetUniformUInt("clusterCount", ClusterCount);
			Pipelines.ClusteredLights.Bind();
			glDispatchCompute((ClusterCount + 63U) / 64U, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
		},
		.DeferredLighting =
			[this, &Pipelines, &Dispatch, &Frame](renderer::graph::RenderGraphContext &Context,
												  const renderer::HybridDeferredFrameResources &Resources)
		{
			constexpr uint32 ClusterCount = 32U * 18U * 24U;
			glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(renderer::RendererBinding::FrameConstants),
							 Frame.FrameConstants.Buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Lights), Frame.Lights.Buffer);
			glBindTextureUnit(0, Context.GetTexture(Resources.GBufferBaseColor));
			glBindTextureUnit(1, Context.GetTexture(Resources.GBufferNormalRoughness));
			glBindTextureUnit(2, Context.GetTexture(Resources.GBufferMaterial));
			glBindTextureUnit(3, Context.GetTexture(Resources.Depth));
			glBindTextureUnit(4, Context.GetTexture(Resources.DirectionalShadowAtlas));
			glBindTextureUnit(5, Context.GetTexture(Resources.SpotShadowAtlas));
			glBindTextureUnit(6, Context.GetTexture(Resources.PointShadowArray));
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ShadowData), Frame.ShadowData.Buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ClusterHeaders),
							 Context.GetBuffer(Resources.ClusterHeaders));
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ClusterIndices),
							 Context.GetBuffer(Resources.ClusterIndices));
			Pipelines.DeferredLighting.SetUniformUInt("lightCount",
													  static_cast<uint32>(this->LightBufferManager.GetTotalLightSourceCount()));
			Pipelines.DeferredLighting.SetUniformUInt("clusterCount", ClusterCount);
			glBindImageTexture(0, Context.GetTexture(Resources.HDRLighting), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
			Dispatch(Pipelines.DeferredLighting, Context, Resources.HDRLighting);
			if (this->HeadlessPresentationValidation)
				this->ValidateHeadlessColorCoverage(Context.GetTexture(Resources.HDRLighting), Context.GetExtent(Resources.HDRLighting),
													"deferred HDR");
		},
		.WeightedOIT =
			[&Pipelines, &DrawBatches](renderer::graph::RenderGraphContext &Context, const renderer::HybridDeferredFrameResources &)
		{ DrawBatches(Context, Pipelines.TransparentOIT, renderer::RenderPassClass::Transparency); },
		.OITComposition =
			[&Pipelines, &Dispatch](renderer::graph::RenderGraphContext &Context, const renderer::HybridDeferredFrameResources &Resources)
		{
			glBindTextureUnit(0, Context.GetTexture(Resources.HDRLighting));
			glBindTextureUnit(1, Context.GetTexture(Resources.TransparencyAccumulation));
			glBindTextureUnit(2, Context.GetTexture(Resources.TransparencyRevealage));
			glBindImageTexture(0, Context.GetTexture(Resources.CompositedHDR), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
			Dispatch(Pipelines.OITComposition, Context, Resources.CompositedHDR);
		},
		.TemporalAA =
			[this, &Pipelines, &Dispatch](renderer::graph::RenderGraphContext &Context,
										  const renderer::HybridDeferredFrameResources &Resources)
		{
			const renderer::graph::Extent2D Extent = Context.GetExtent(Resources.TAAResolved);
			const bool HistoryMatchesExtent =
				TemporalHistoryValid && TemporalHistoryExtent.Width == Extent.Width && TemporalHistoryExtent.Height == Extent.Height;
			glBindTextureUnit(0, Context.GetTexture(Resources.CompositedHDR));
			glBindTextureUnit(1, Context.GetTexture(Resources.TAAHistoryRead));
			glBindTextureUnit(2, Context.GetTexture(Resources.Velocity));
			Pipelines.TemporalAA.SetUniformUInt("historyValid", HistoryMatchesExtent ? 1U : 0U);
			glBindImageTexture(0, Context.GetTexture(Resources.TAAHistoryWrite), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
			glBindImageTexture(1, Context.GetTexture(Resources.TAAResolved), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
			Dispatch(Pipelines.TemporalAA, Context, Resources.TAAResolved);
			if (this->HeadlessPresentationValidation)
				this->ValidateHeadlessColorCoverage(Context.GetTexture(Resources.TAAResolved), Extent, "TAA resolve");
		},
		.ExposureAndBloom =
			[this, &Pipelines](renderer::graph::RenderGraphContext &Context, const renderer::HybridDeferredFrameResources &Resources)
		{
			const renderer::graph::Extent2D Extent = Context.GetExtent(Resources.Bloom);
			const uint32 MipCount = static_cast<uint32>(std::bit_width(std::max(Extent.Width, Extent.Height)));
			const GLuint BloomTexture = Context.GetTexture(Resources.Bloom);
			Pipelines.AutoExposure.Bind();
			glBindTextureUnit(0, Context.GetTexture(Resources.TAAResolved));
			glBindImageTexture(0, Context.GetTexture(Resources.Exposure), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
			Pipelines.AutoExposure.SetUniformUInt("historyValid", ExposureHistoryValid ? 1U : 0U);
			glDispatchCompute(1, 1, 1);
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
			Pipelines.Bloom.Bind();
			for (uint32 Mip = 0; Mip < MipCount; ++Mip)
			{
				const uint32 Width = std::max(1U, Extent.Width >> Mip);
				const uint32 Height = std::max(1U, Extent.Height >> Mip);
				glBindTextureUnit(0, Mip == 0 ? Context.GetTexture(Resources.TAAResolved) : BloomTexture);
				glBindImageTexture(0, BloomTexture, static_cast<GLint>(Mip), GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
				Pipelines.Bloom.SetUniformUInt("sourceMip", Mip == 0 ? 0U : Mip - 1U);
				Pipelines.Bloom.SetUniformUInt("operation", 0U);
				glDispatchCompute((Width + 7U) / 8U, (Height + 7U) / 8U, 1);
				glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
			}
			for (uint32 Mip = MipCount - 1U; Mip > 0U; --Mip)
			{
				const uint32 OutputMip = Mip - 1U;
				const uint32 Width = std::max(1U, Extent.Width >> OutputMip);
				const uint32 Height = std::max(1U, Extent.Height >> OutputMip);
				glBindTextureUnit(0, BloomTexture);
				glBindImageTexture(0, BloomTexture, static_cast<GLint>(OutputMip), GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
				Pipelines.Bloom.SetUniformUInt("sourceMip", Mip);
				Pipelines.Bloom.SetUniformUInt("operation", 1U);
				glDispatchCompute((Width + 7U) / 8U, (Height + 7U) / 8U, 1);
				glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
			}
		},
		.ToneMapAndPresent =
			[&Pipelines](renderer::graph::RenderGraphContext &Context, const renderer::HybridDeferredFrameResources &Resources)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glBindTextureUnit(0, Context.GetTexture(Resources.TAAResolved));
			glBindTextureUnit(1, Context.GetTexture(Resources.Bloom));
			glBindTextureUnit(2, Context.GetTexture(Resources.Exposure));
			Pipelines.ToneMap.Bind();
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}};
	(void)HybridFrameGraph.Build(RenderGraph, Inputs, Callbacks);
	RenderGraph.Compile();
	RenderGraph.Execute();
	this->ValidateHeadlessPresentation(Window);
	HierarchicalDepthHistoryExtent = Extent;
	HierarchicalDepthHistoryValid = true;
	TemporalHistoryExtent = Extent;
	TemporalHistoryValid = true;
	ExposureHistoryValid = true;
	PreviousCameraPosition = Camera.Position;
	PreviousCameraFront = Camera.Front;
	HasPreviousCameraState = true;
	FrameResources->Retire(SceneCollection.ReleaseAssetPins());
	++FrameNumber;
	CollectingFrame = false;
	SceneCollection.Clear();
	ObjectsDrawn = 0;
	Recovery.Release();
}

void OpenGLRenderer::RecoverFailedFrame() noexcept
{
	std::vector<resource::AssetPtr<resource::Asset>> Pins = this->SceneCollection.ReleaseAssetPins();
	if (this->FrameResources != nullptr && this->FrameResources->IsAcquired())
	{
		try
		{
			this->FrameResources->Retire(std::move(Pins));
		}
		catch (...)
		{
			this->FrameResources->Abandon(std::move(Pins));
		}
	}
	this->CollectingFrame = false;
	this->SceneCollection.Clear();
	this->ObjectsDrawn = 0;
	this->HierarchicalDepthHistoryValid = false;
	this->TemporalHistoryValid = false;
	this->ExposureHistoryValid = false;
}

void OpenGLRenderer::EnableCulling() const
{
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
}
void OpenGLRenderer::DisableCulling() const
{
	glDisable(GL_CULL_FACE);
}

void OpenGLRenderer::ValidateHeadlessDepthCoverage(GLuint DepthTexture, renderer::graph::Extent2D Extent) const
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

void OpenGLRenderer::ValidateHeadlessColorCoverage(GLuint ColorTexture, renderer::graph::Extent2D Extent, string_view Stage) const
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

void OpenGLRenderer::ValidateHeadlessPresentation(core::Window &Window)
{
	if (!this->HeadlessPresentationValidation || this->PresentationValidated)
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
	std::cerr << "Headless presentation validation: " << LitPixelCount << " non-black pixels\n";
	this->PresentationValidated = true;
	// In the opt-in hidden validation mode, one successfully presented frame is
	// the test result. End the application so automated validation cannot mask
	// a failure behind an externally forced timeout.
	Window.RequestClose();
}
