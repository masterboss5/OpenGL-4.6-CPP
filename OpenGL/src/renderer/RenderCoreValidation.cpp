#include "RenderCoreValidation.h"

#include "RenderGraph.h"
#include "RendererGpuTypes.h"
#include "ScenePreparation.h"
#include "src/core/events/EventDispatcher.h"
#include "LightBufferManager.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/pipeline/device/Device.h"
#include "src/pipeline/shader/ShaderLibrary.h"
#include "src/pipeline/vertex/VertexDescriptor.h"
#include "src/resource/asset/AssetManager.h"
#include "src/resource/asset/importer/AssetImporter.h"
#include "src/scene/Scene.h"
#include "src/scene/SceneCollection.h"
#include "src/scene/SceneCommandBuffer.h"

#include <ext/matrix_clip_space.hpp>
#include <gtc/matrix_transform.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace renderer::validation
{
namespace
{
std::atomic<uint32> ValidationImportCount{0};
std::atomic<bool> FailValidationAsset{false};

class ValidationAsset final : public resource::Asset
{
  public:
	ValidationAsset() : Asset(util::UUID::GenerateRandomUUID())
	{
	}
};

class ReservingValidationImporter final : public resource::importer::AssetImporter
{
  public:
	[[nodiscard]] bool CanImport(const std::filesystem::path &) const override
	{
		return true;
	}
	[[nodiscard]] resource::AssetType GetAssetType() const noexcept override
	{
		return resource::AssetType::ShaderSource;
	}
	[[nodiscard]] resource::importer::AssetImportResult ImportCPU(const std::filesystem::path &Path,
																  resource::importer::AssetImportContext &Context) const override
	{
		ValidationImportCount.fetch_add(1, std::memory_order_relaxed);
		if (Path.filename() == "Failing.shader" && FailValidationAsset.load(std::memory_order_relaxed))
		{
			throw resource::importer::AssetContentValidationException(resource::AssetType::ShaderSource, Path,
																	  "Injected deterministic reload failure");
		}
		if (Path.filename() == "Fullscreen.vert")
			(void)Context.Reserve<ValidationAsset>(resource::AssetType::ShaderSource, Path.parent_path() / "ToneMap.frag");
		std::vector<resource::AssetDependency> Dependencies;
		if (Path.filename() == "Dependent.shader")
			Dependencies.push_back({resource::AssetType::ShaderSource, Path.parent_path() / "Dependency.inc"});
		return resource::importer::AssetImportResult(resource::AssetPtr<ValidationAsset>::Make(), std::move(Dependencies));
	}
};

class TemporaryValidationDirectory final
{
  public:
	TemporaryValidationDirectory()
		: Path(std::filesystem::temp_directory_path() / ("OpenGL-RenderCoreValidation-" + util::UUID::GenerateRandomUUID().ToString()))
	{
		std::filesystem::create_directories(this->Path);
	}
	~TemporaryValidationDirectory()
	{
		std::error_code Error;
		std::filesystem::remove_all(this->Path, Error);
	}
	[[nodiscard]] const std::filesystem::path &GetPath() const noexcept
	{
		return this->Path;
	}

  private:
	std::filesystem::path Path;
};

void WriteValidationFile(const std::filesystem::path &Path, const string_view Contents)
{
	std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
	if (!Stream)
		throw std::runtime_error("Could not create deterministic asset validation fixture");
	Stream.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
	if (!Stream)
		throw std::runtime_error("Could not write deterministic asset validation fixture");
}

void AdvanceValidationWriteTime(const std::filesystem::path &Path)
{
	std::error_code Error;
	const auto Current = std::filesystem::last_write_time(Path, Error);
	if (Error)
		throw std::runtime_error("Could not inspect deterministic asset validation fixture timestamp");
	std::filesystem::last_write_time(Path, Current + std::chrono::seconds(2), Error);
	if (Error)
		throw std::runtime_error("Could not advance deterministic asset validation fixture timestamp");
}

void Require(bool Condition, string_view Diagnostic)
{
	if (!Condition)
		throw std::runtime_error("Render-core deterministic validation failed: " + string(Diagnostic));
}

void ValidateGraphDependencies(pipeline::device::Device &Device)
{
	graph::RenderGraph InvalidGraph(Device);
	InvalidGraph.BeginFrame({16, 16});
	const graph::TextureHandle UnwrittenTexture = InvalidGraph.CreateTexture({.DebugName = "UnwrittenTexture", .Extent = {16, 16}});
	(void)InvalidGraph.AddPass({.Name = "InvalidRead",
								.Queue = graph::PassQueue::Compute,
								.ReadTextures = {UnwrittenTexture},
								.Execute = [](graph::RenderGraphContext &) {}});
	bool RejectedUnwrittenRead = false;
	try
	{
		InvalidGraph.Compile();
	}
	catch (const std::logic_error &)
	{
		RejectedUnwrittenRead = true;
	}
	Require(RejectedUnwrittenRead, "a render graph read-before-write dependency was accepted");

	graph::RenderGraph ValidGraph(Device);
	ValidGraph.BeginFrame({16, 16});
	const graph::TextureHandle ProducedTexture = ValidGraph.CreateTexture({.DebugName = "ProducedTexture", .Extent = {16, 16}});
	(void)ValidGraph.AddPass({.Name = "Produce",
							  .Queue = graph::PassQueue::Compute,
							  .WriteTextures = {ProducedTexture},
							  .Execute = [](graph::RenderGraphContext &) {}});
	(void)ValidGraph.AddPass({.Name = "Consume",
							  .Queue = graph::PassQueue::Compute,
							  .ReadTextures = {ProducedTexture},
							  .Execute = [](graph::RenderGraphContext &) {}});
	ValidGraph.Compile();
	Require(ValidGraph.IsCompiled(), "a valid producer-consumer graph did not compile");
}

void ValidateGraphResourceLifetimes(pipeline::device::Device &Device)
{
	// Load/store discard declarations must execute as real framebuffer
	// invalidations. A complete no-op graphics pass is sufficient here:
	// RenderGraph checks all OpenGL errors after that pass.
	graph::RenderGraph DiscardGraph(Device);
	DiscardGraph.BeginFrame({16, 16});
	const graph::TextureHandle DiscardTexture = DiscardGraph.CreateTexture({.DebugName = "DiscardAttachment", .Extent = {16, 16}});
	(void)DiscardGraph.AddPass(
		{.Name = "DiscardAttachmentPass",
		 .Queue = graph::PassQueue::Graphics,
		 .ColorAttachments = {{.Texture = DiscardTexture, .Load = graph::LoadOperation::Discard, .Store = graph::StoreOperation::Discard}},
		 .Execute = [](graph::RenderGraphContext &) {}});
	DiscardGraph.Compile();
	DiscardGraph.Execute();

	// The graph must also allocate and attach a native multisample target;
	// a later resolve pass can consume it without falling back to a hidden
	// single-sample allocation.
	graph::RenderGraph MultisampleGraph(Device);
	MultisampleGraph.BeginFrame({16, 16});
	const graph::TextureHandle MultisampleColor =
		MultisampleGraph.CreateTexture({.DebugName = "MultisampleColor",
										.Extent = {16, 16},
										.Dimension = graph::TextureDimension::Texture2DMultisample,
										.SampleCount = 4});
	(void)MultisampleGraph.AddPass({.Name = "ClearMultisampleColor",
									.Queue = graph::PassQueue::Graphics,
									.ColorAttachments = {{.Texture = MultisampleColor, .Load = graph::LoadOperation::Clear}},
									.Execute = [](graph::RenderGraphContext &) {}});
	MultisampleGraph.Compile();
	MultisampleGraph.Execute();

	// Transient resources whose lifetimes do not overlap should share a
	// physical allocation. This is the graph's transient-memory aliasing
	// contract, not merely a cache optimization.
	graph::RenderGraph AliasedGraph(Device);
	AliasedGraph.BeginFrame({16, 16});
	const graph::TextureHandle First = AliasedGraph.CreateTexture({.DebugName = "AliasedFirst", .Extent = {16, 16}});
	const graph::TextureHandle Second = AliasedGraph.CreateTexture({.DebugName = "AliasedSecond", .Extent = {16, 16}});
	GLuint FirstPhysicalTexture = 0;
	GLuint SecondPhysicalTexture = 0;
	(void)AliasedGraph.AddPass({.Name = "WriteAliasedFirst",
								.Queue = graph::PassQueue::Compute,
								.WriteTextures = {First},
								.Execute = [&FirstPhysicalTexture, First](graph::RenderGraphContext &Context)
								{ FirstPhysicalTexture = Context.GetTexture(First); }});
	(void)AliasedGraph.AddPass({.Name = "WriteAliasedSecond",
								.Queue = graph::PassQueue::Compute,
								.WriteTextures = {Second},
								.Execute = [&SecondPhysicalTexture, Second](graph::RenderGraphContext &Context)
								{ SecondPhysicalTexture = Context.GetTexture(Second); }});
	AliasedGraph.Compile();
	AliasedGraph.Execute();
	Require(FirstPhysicalTexture != 0 && FirstPhysicalTexture == SecondPhysicalTexture, "non-overlapping transient textures did not alias");

	// A consumer extends the first texture's lifetime through the second
	// pass, so reusing its allocation for the second texture would corrupt
	// the read. The graph must keep them physically distinct.
	graph::RenderGraph OverlappingGraph(Device);
	OverlappingGraph.BeginFrame({16, 16});
	const graph::TextureHandle Producer = OverlappingGraph.CreateTexture({.DebugName = "OverlappingProducer", .Extent = {16, 16}});
	const graph::TextureHandle Concurrent = OverlappingGraph.CreateTexture({.DebugName = "OverlappingConcurrent", .Extent = {16, 16}});
	GLuint ProducerPhysicalTexture = 0;
	GLuint ConcurrentPhysicalTexture = 0;
	(void)OverlappingGraph.AddPass({.Name = "WriteProducer",
									.Queue = graph::PassQueue::Compute,
									.WriteTextures = {Producer},
									.Execute = [&ProducerPhysicalTexture, Producer](graph::RenderGraphContext &Context)
									{ ProducerPhysicalTexture = Context.GetTexture(Producer); }});
	(void)OverlappingGraph.AddPass({.Name = "ReadProducerWriteConcurrent",
									.Queue = graph::PassQueue::Compute,
									.ReadTextures = {Producer},
									.WriteTextures = {Concurrent},
									.Execute = [&ConcurrentPhysicalTexture, Concurrent](graph::RenderGraphContext &Context)
									{ ConcurrentPhysicalTexture = Context.GetTexture(Concurrent); }});
	OverlappingGraph.Compile();
	OverlappingGraph.Execute();
	Require(ProducerPhysicalTexture != 0 && ConcurrentPhysicalTexture != 0 && ProducerPhysicalTexture != ConcurrentPhysicalTexture,
			"overlapping transient textures aliased unsafely");

	// Persistent history survives equivalent frames but an extent change
	// receives a distinct allocation. OpenGLRenderer compares this extent
	// before enabling Hi-Z/TAA history, so resize cannot sample old data.
	graph::RenderGraph HistoryGraph(Device);
	HistoryGraph.BeginFrame({16, 16});
	const graph::TextureHandle InitialHistory =
		HistoryGraph.CreateTexture({.DebugName = "History", .Format = graph::TextureFormat::R32Float, .Persistent = true});
	const graph::TextureHandle IndependentHistory =
		HistoryGraph.CreateTexture({.DebugName = "IndependentHistory", .Format = graph::TextureFormat::R32Float, .Persistent = true});
	GLuint InitialHistoryTexture = 0;
	GLuint IndependentHistoryTexture = 0;
	(void)HistoryGraph.AddPass({.Name = "WriteInitialHistory",
								.Queue = graph::PassQueue::Compute,
								.WriteTextures = {InitialHistory, IndependentHistory},
								.Execute = [&InitialHistoryTexture, &IndependentHistoryTexture, InitialHistory,
											IndependentHistory](graph::RenderGraphContext &Context)
								{
									InitialHistoryTexture = Context.GetTexture(InitialHistory);
									IndependentHistoryTexture = Context.GetTexture(IndependentHistory);
								}});
	HistoryGraph.Compile();
	HistoryGraph.Execute();
	Require(InitialHistoryTexture != 0 && IndependentHistoryTexture != 0 && InitialHistoryTexture != IndependentHistoryTexture,
			"independent persistent histories aliased the same physical texture");
	HistoryGraph.BeginFrame({16, 16});
	const graph::TextureHandle MatchingHistory =
		HistoryGraph.CreateTexture({.DebugName = "History", .Format = graph::TextureFormat::R32Float, .Persistent = true});
	GLuint MatchingHistoryTexture = 0;
	(void)HistoryGraph.AddPass({.Name = "ReuseHistory",
								.Queue = graph::PassQueue::Compute,
								.WriteTextures = {MatchingHistory},
								.Execute = [&MatchingHistoryTexture, MatchingHistory](graph::RenderGraphContext &Context)
								{ MatchingHistoryTexture = Context.GetTexture(MatchingHistory); }});
	HistoryGraph.Compile();
	HistoryGraph.Execute();
	Require(InitialHistoryTexture != 0 && InitialHistoryTexture == MatchingHistoryTexture,
			"persistent history was not retained across equivalent frames");
	HistoryGraph.BeginFrame({32, 32});
	const graph::TextureHandle ResizedHistory =
		HistoryGraph.CreateTexture({.DebugName = "History", .Format = graph::TextureFormat::R32Float, .Persistent = true});
	GLuint ResizedHistoryTexture = 0;
	(void)HistoryGraph.AddPass({.Name = "WriteResizedHistory",
								.Queue = graph::PassQueue::Compute,
								.WriteTextures = {ResizedHistory},
								.Execute = [&ResizedHistoryTexture, ResizedHistory](graph::RenderGraphContext &Context)
								{ ResizedHistoryTexture = Context.GetTexture(ResizedHistory); }});
	HistoryGraph.Compile();
	HistoryGraph.Execute();
	Require(ResizedHistoryTexture != 0 && ResizedHistoryTexture != InitialHistoryTexture,
			"resized persistent history reused an incompatible allocation");
}

void ValidateGPUVisibilityCompaction(pipeline::device::Device &Device)
{
	// Exercise the actual runtime shaders with 512 one-instance batches.
	// This crosses the 256-entry first-level scan block boundary and proves
	// cull -> block scan -> block-prefix -> finalize -> scatter produces
	// contiguous DrawElementsIndirectCommand base instances without CPU
	// readback between phases.
	constexpr uint32 ScratchCapacity = 65'536;
	constexpr uint32 CandidateCount = 512;
	constexpr uint32 ScanBlockCount = (CandidateCount + 255U) / 256U;
	GLuint FrameBuffer = 0;
	GLuint CandidateBuffer = 0;
	GLuint VisibleBuffer = 0;
	GLuint CommandBuffer = 0;
	GLuint ScratchBuffer = 0;
	GLuint HierarchyTexture = 0;
	try
	{
		resource::AssetManager Assets;
		pipeline::shader::ShaderLibrary Shaders(Device, Assets);
		const uint32 CullPipeline =
			Shaders.CreateComputePipeline({.Compute = {.Path = "shader/Visibility.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		const uint32 PrefixPipeline = Shaders.CreateComputePipeline(
			{.Compute = {.Path = "shader/VisibilityPrefixScan.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		const uint32 BlockPrefixPipeline = Shaders.CreateComputePipeline(
			{.Compute = {.Path = "shader/VisibilityBlockPrefixScan.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		const uint32 FinalizePipeline = Shaders.CreateComputePipeline(
			{.Compute = {.Path = "shader/VisibilityFinalize.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		const uint32 ScatterPipeline = Shaders.CreateComputePipeline(
			{.Compute = {.Path = "shader/VisibilityScatter.comp", .Stage = pipeline::shader::ShaderStage::Compute}});

		std::vector<PreparedInstance> Candidates(CandidateCount);
		std::vector<RenderCommand> Commands(CandidateCount);
		for (uint32 Index = 0; Index < CandidateCount; ++Index)
		{
			Candidates[Index] = {.Transform = glm::mat4(1.0f),
								 .PreviousTransform = glm::mat4(1.0f),
								 .WorldBounds = glm::vec4(0.0f, 0.0f, 0.0f, 0.1f),
								 .MaterialIndex = 0,
								 .ObjectID = Index,
								 .BatchIndex = Index,
								 .Flags = 0};
			Commands[Index] = {.IndexCount = 3, .InstanceCount = 0, .FirstIndex = 0, .BaseVertex = 0, .BaseInstance = 0};
		}
		GPUFrameConstants FrameConstants{};
		FrameConstants.Projection = glm::mat4(1.0f);
		FrameConstants.View = glm::mat4(1.0f);
		FrameConstants.ViewProjection = glm::mat4(1.0f);
		FrameConstants.PreviousViewProjection = glm::mat4(1.0f);
		FrameConstants.InverseViewProjection = glm::mat4(1.0f);
		FrameConstants.RenderExtentAndFar = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
		glCreateBuffers(1, &FrameBuffer);
		glNamedBufferStorage(FrameBuffer, sizeof(GPUFrameConstants), &FrameConstants, 0);
		glCreateBuffers(1, &CandidateBuffer);
		glNamedBufferStorage(CandidateBuffer, static_cast<GLsizeiptr>(Candidates.size() * sizeof(PreparedInstance)), Candidates.data(), 0);
		glCreateBuffers(1, &VisibleBuffer);
		glNamedBufferStorage(VisibleBuffer, static_cast<GLsizeiptr>(Candidates.size() * sizeof(PreparedInstance)), nullptr,
							 GL_DYNAMIC_STORAGE_BIT);
		glCreateBuffers(1, &CommandBuffer);
		glNamedBufferStorage(CommandBuffer, static_cast<GLsizeiptr>(Commands.size() * sizeof(RenderCommand)), Commands.data(),
							 GL_DYNAMIC_STORAGE_BIT);
		glCreateBuffers(1, &ScratchBuffer);
		glNamedBufferStorage(ScratchBuffer, static_cast<GLsizeiptr>((ScratchCapacity * 4U + 512U) * sizeof(uint32)), nullptr,
							 GL_DYNAMIC_STORAGE_BIT);
		glCreateTextures(GL_TEXTURE_2D, 1, &HierarchyTexture);
		glTextureStorage2D(HierarchyTexture, 1, GL_R32F, 1, 1);

		const uint32 Zero = 0;
		glClearNamedBufferData(ScratchBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &Zero);
		glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(RendererBinding::FrameConstants), FrameBuffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::Candidates), CandidateBuffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::Instances), VisibleBuffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::VisibilityScratch), ScratchBuffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::IndirectCommands), CommandBuffer);
		glBindTextureUnit(0, HierarchyTexture);

		auto &Cull = Shaders.GetComputePipeline(CullPipeline);
		Cull.SetUniformUInt("candidateCount", CandidateCount);
		Cull.SetUniformUInt("pyramidMipCount", 1);
		Cull.SetUniformUInt("historyValid", 0);
		Cull.SetUniformUInt("scratchCapacity", ScratchCapacity);
		Cull.Bind();
		glDispatchCompute((CandidateCount + 63U) / 64U, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

		auto &Prefix = Shaders.GetComputePipeline(PrefixPipeline);
		Prefix.SetUniformUInt("batchCount", CandidateCount);
		Prefix.SetUniformUInt("scratchCapacity", ScratchCapacity);
		Prefix.Bind();
		glDispatchCompute(ScanBlockCount, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

		auto &BlockPrefix = Shaders.GetComputePipeline(BlockPrefixPipeline);
		BlockPrefix.SetUniformUInt("blockCount", ScanBlockCount);
		BlockPrefix.SetUniformUInt("scratchCapacity", ScratchCapacity);
		BlockPrefix.Bind();
		glDispatchCompute(1, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

		auto &Finalize = Shaders.GetComputePipeline(FinalizePipeline);
		Finalize.SetUniformUInt("batchCount", CandidateCount);
		Finalize.SetUniformUInt("scratchCapacity", ScratchCapacity);
		Finalize.Bind();
		glDispatchCompute(ScanBlockCount, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

		auto &Scatter = Shaders.GetComputePipeline(ScatterPipeline);
		Scatter.SetUniformUInt("candidateCount", CandidateCount);
		Scatter.SetUniformUInt("scratchCapacity", ScratchCapacity);
		Scatter.Bind();
		glDispatchCompute((CandidateCount + 63U) / 64U, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
		Device.CheckErrors("deterministic GPU visibility validation");

		std::vector<RenderCommand> CompactCommands(CandidateCount);
		std::vector<PreparedInstance> CompactInstances(CandidateCount);
		glGetNamedBufferSubData(CommandBuffer, 0, static_cast<GLsizeiptr>(CompactCommands.size() * sizeof(RenderCommand)),
								CompactCommands.data());
		glGetNamedBufferSubData(VisibleBuffer, 0, static_cast<GLsizeiptr>(CompactInstances.size() * sizeof(PreparedInstance)),
								CompactInstances.data());
		Device.CheckErrors("GPU visibility validation readback");
		for (uint32 Index = 0; Index < CandidateCount; ++Index)
		{
			Require(CompactCommands[Index].InstanceCount == 1 && CompactCommands[Index].BaseInstance == Index,
					"parallel prefix scan produced an invalid compact indirect range");
			Require(CompactInstances[Index].BatchIndex == Index && CompactInstances[Index].ObjectID == Index,
					"parallel visibility scatter produced an invalid compact instance stream");
		}
	}
	catch (...)
	{
		if (HierarchyTexture != 0)
			glDeleteTextures(1, &HierarchyTexture);
		if (ScratchBuffer != 0)
			glDeleteBuffers(1, &ScratchBuffer);
		if (CommandBuffer != 0)
			glDeleteBuffers(1, &CommandBuffer);
		if (VisibleBuffer != 0)
			glDeleteBuffers(1, &VisibleBuffer);
		if (CandidateBuffer != 0)
			glDeleteBuffers(1, &CandidateBuffer);
		if (FrameBuffer != 0)
			glDeleteBuffers(1, &FrameBuffer);
		throw;
	}
	if (HierarchyTexture != 0)
		glDeleteTextures(1, &HierarchyTexture);
	if (ScratchBuffer != 0)
		glDeleteBuffers(1, &ScratchBuffer);
	if (CommandBuffer != 0)
		glDeleteBuffers(1, &CommandBuffer);
	if (VisibleBuffer != 0)
		glDeleteBuffers(1, &VisibleBuffer);
	if (CandidateBuffer != 0)
		glDeleteBuffers(1, &CandidateBuffer);
	if (FrameBuffer != 0)
		glDeleteBuffers(1, &FrameBuffer);
}

void ValidateScenePreparation(pipeline::device::Device &Device)
{
	(void)Device;
	const VertexDescriptor Descriptor({{.BindingIndex = 0, .StrideInBytes = sizeof(float32) * 3U}},
									  {{.Semantic = "POSITION", .Location = 0, .BindingIndex = 0, .ComponentCount = 3}});
	const util::UUID MaterialGeneration = util::UUID::GenerateRandomUUID();
	RenderItem VisibleItem{.VertexArray = 1,
						   .VertexDescriptor = &Descriptor,
						   .IndexCount = 3,
						   .MaterialGeneration = MaterialGeneration,
						   .Transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f)),
						   .PreviousTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)),
						   .WorldBounds = glm::vec4(0.0f, 0.0f, -5.0f, 1.0f),
						   .ObjectID = 17};
	SceneCollection VisibleScene;
	VisibleScene.BeginFrame(1);
	VisibleScene.Submit(VisibleItem);
	RenderItem SecondMaterialItem = VisibleItem;
	SecondMaterialItem.MaterialGeneration = util::UUID::GenerateRandomUUID();
	SecondMaterialItem.ObjectID = 19;
	VisibleScene.Submit(SecondMaterialItem);
	VisibleScene.Seal();
	ScenePreparation Preparation;
	const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
	const RenderPreparationResult Visible = Preparation.Prepare(VisibleScene, Projection, 0, 1);
	Require(Visible.CandidateInstances.size() == 2 && Visible.Batches.size() == 1 && Visible.CandidateCommands.size() == 1,
			"compatible mesh sections with different materials did not share one batch");
	Require(Visible.Materials.size() == 2, "per-instance materials were lost while merging a compatible batch");
	Require(Visible.CandidateCommands.front().InstanceCount == 2 && Visible.CandidateCommands.front().IndexCount == 3,
			"indirect command metadata is invalid");
	Require(Visible.CandidateInstances.front().ObjectID == 17, "prepared instance lost its stable object identifier");
	Require(Visible.CandidateInstances.front().PreviousTransform[3].z == -4.0f,
			"prepared instance lost its previous transform required for motion vectors");

	SceneCollection RangedScene;
	RangedScene.BeginFrame(2);
	RangedScene.Submit(VisibleItem);
	RenderItem SecondRange = VisibleItem;
	SecondRange.FirstIndex = 3;
	SecondRange.ObjectID = 20;
	RangedScene.Submit(SecondRange);
	RangedScene.Seal();
	const RenderPreparationResult Ranged = Preparation.Prepare(RangedScene, Projection, 0, 1);
	Require(Ranged.Batches.size() == 2 && Ranged.CandidateCommands.size() == 2,
			"different indexed mesh sections were merged into one indirect batch");

	RenderItem CulledItem = VisibleItem;
	CulledItem.Transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f));
	CulledItem.WorldBounds = glm::vec4(0.0f, 0.0f, 5.0f, 1.0f);
	CulledItem.ObjectID = 18;
	SceneCollection CulledScene;
	CulledScene.BeginFrame(3);
	CulledScene.Submit(CulledItem);
	CulledScene.Seal();
	const RenderPreparationResult Culled = Preparation.Prepare(CulledScene, Projection, 0, 1);
	Require(Culled.CandidateInstances.empty() && Culled.Batches.empty() && Culled.CandidateCommands.empty(),
			"frustum culling retained geometry behind the camera");
	const RenderPreparationResult ShadowCasters = Preparation.Prepare(CulledScene, Projection, 0, 1, false);
	Require(ShadowCasters.CandidateInstances.size() == 1 && ShadowCasters.Batches.size() == 1,
			"all-caster shadow preparation discarded an off-camera shadow caster");
}

void ValidateSceneStorageAndCommands()
{
	world::Scene Scene({.Objects = 16, .ComponentsPerType = 16});
	const world::ObjectHandle First = Scene.CreateObject();
	const world::ObjectHandle Second = Scene.CreateObject();
	world::SceneCommandBuffer Commands;
	Commands.AddComponent<components::CObjectTransformComponent>(First);
	Commands.AddComponent<components::CObjectTransformComponent>(Second);
	Require(Commands.Size() == 2, "scene command buffer lost queued component additions");
	Commands.Execute(Scene);
	const auto FirstTransform = Scene.GetComponent<components::CObjectTransformComponent>(First);
	const auto SecondTransform = Scene.GetComponent<components::CObjectTransformComponent>(Second);
	Require(FirstTransform.IsValid() && SecondTransform.IsValid(), "scene command buffer failed to attach components");
	Commands.RemoveComponent<components::CObjectTransformComponent>(First);
	Commands.Execute(Scene);
	Require(!Scene.GetComponent<components::CObjectTransformComponent>(First).IsValid(),
			"scene command buffer failed to remove a component");
	{
		auto Access = Scene.Read();
		Require(Access.Resolve(SecondTransform).GetOwner() == Second,
				"dense component relocation invalidated a generational component handle");
	}
	Commands.DestroyObject(Second);
	Commands.Execute(Scene);
	Require(!Scene.Contains(Second), "scene command buffer failed to destroy an object");
	bool StaleRejected = false;
	try
	{
		auto Access = Scene.Read();
		(void)Access.Resolve(SecondTransform);
	}
	catch (const world::InvalidComponentHandleException &)
	{
		StaleRejected = true;
	}
	Require(StaleRejected, "destroyed component handle was not rejected by generation validation");

	const world::ObjectHandle SurvivingObject = Scene.CreateObject();
	Commands.DestroyObject(Second);
	Commands.AddComponent<components::CObjectTransformComponent>(SurvivingObject);
	bool CommandFailureReported = false;
	try
	{
		Commands.Execute(Scene);
	}
	catch (const world::SceneCommandExecutionException &Exception)
	{
		CommandFailureReported = Exception.GetCommandIndex() == 0;
	}
	Require(CommandFailureReported, "scene command failure did not report the failing command index");
	Require(Commands.Size() == 1, "scene command failure discarded the unexecuted command tail");
	Commands.Execute(Scene);
	Require(Scene.GetComponent<components::CObjectTransformComponent>(SurvivingObject).IsValid(),
			"requeued scene command tail did not execute after a prior failure");
}

void ValidateTransactionalLightUploads()
{
	LightBufferManager Lights(2);
	const std::vector<PointLightSource> Points{{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f), 1.0f, 0.1f, 0.01f}};
	Lights.UploadLightSources(Points);
	Require(Lights.GetGPURecords().size() == 1 && Lights.GetPointLights().size() == 1,
			"initial transactional light upload did not publish its records");
	const SpotLightSource Spot(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 0.9f, 0.8f, glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f),
							   1.0f, 0.1f, 0.01f);
	const std::vector<SpotLightSource> ExcessSpots{Spot, Spot};
	bool CapacityRejected = false;
	try
	{
		Lights.UploadLightSources(ExcessSpots);
	}
	catch (const std::runtime_error &)
	{
		CapacityRejected = true;
	}
	Require(CapacityRejected, "light upload exceeding the unified capacity was accepted");
	Require(Lights.GetGPURecords().size() == 1 && Lights.GetPointLights().size() == 1 && Lights.GetSpotLights().empty(),
			"failed light upload mutated the last successfully published light state");
}

void ValidateEventListenerMutation()
{
	core::EventDispatcher<uint32> Dispatcher;
	core::EventSubscription SelfRemoving;
	bool FirstCalled = false;
	bool AddedCalled = false;
	std::vector<core::EventSubscription> AddedSubscriptions;
	SelfRemoving = Dispatcher.Subscribe(10,
										[&](const uint32 &)
										{
											FirstCalled = true;
											SelfRemoving.Reset();
											AddedSubscriptions.push_back(Dispatcher.Subscribe(0,
																							  [&](const uint32 &)
																							  {
																								  AddedCalled = true;
																								  return core::EventPropagation::Continue;
																							  }));
											return core::EventPropagation::Continue;
										});
	Require(Dispatcher.Dispatch(1) == core::EventPropagation::Continue && FirstCalled && !SelfRemoving.IsSubscribed(),
			"listener self-removal invalidated the executing event callback");
	Require(!AddedCalled, "listener added during dispatch executed before the next event");
	(void)Dispatcher.Dispatch(2);
	Require(AddedCalled, "listener added during dispatch was not retained for the next event");
}

void ValidateReservedAssetLoading()
{
	ValidationImportCount.store(0, std::memory_order_relaxed);
	resource::AssetManager Assets;
	Assets.AddAssetImporter<ReservingValidationImporter>();
	const std::filesystem::path RootPath = "shader/Fullscreen.vert";
	const std::filesystem::path ReservedPath = "shader/ToneMap.frag";
	auto Root = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, RootPath);
	Require(Root.Pin() != nullptr && ValidationImportCount.load(std::memory_order_relaxed) == 1,
			"validation root asset did not import exactly once");
	const resource::AssetRecord *ReservedRecord = Assets.GetRecord(resource::AssetType::ShaderSource, ReservedPath);
	Require(ReservedRecord != nullptr && ReservedRecord->GetState() == resource::AssetLoadState::Unloaded,
			"import-context reservation unexpectedly published an unstaged asset");
	auto Reserved = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, ReservedPath);
	Require(Reserved.Pin() != nullptr && ValidationImportCount.load(std::memory_order_relaxed) == 2,
			"an existing unloaded asset reservation was returned without being imported");
}

void ValidateAssetReloadPipeline()
{
	TemporaryValidationDirectory Temporary;
	const std::filesystem::path DependentPath = Temporary.GetPath() / "Dependent.shader";
	const std::filesystem::path DependencyPath = Temporary.GetPath() / "Dependency.inc";
	const std::filesystem::path FailingPath = Temporary.GetPath() / "Failing.shader";
	const std::filesystem::path HealthyPath = Temporary.GetPath() / "Healthy.shader";
	WriteValidationFile(DependentPath, "dependent");
	WriteValidationFile(DependencyPath, "dependency");
	WriteValidationFile(FailingPath, "failing");
	WriteValidationFile(HealthyPath, "healthy");

	FailValidationAsset.store(false, std::memory_order_relaxed);
	resource::AssetManager Assets;
	Assets.AddAssetImporter<ReservingValidationImporter>();
	auto Dependent = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, DependentPath);
	auto Failing = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, FailingPath);
	auto Healthy = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, HealthyPath);
	const uint64 DependentGeneration = Dependent.GetPublishedGeneration();
	const uint64 FailingGeneration = Failing.GetPublishedGeneration();
	const uint64 HealthyGeneration = Healthy.GetPublishedGeneration();

	AdvanceValidationWriteTime(DependencyPath);
	Require(Assets.ReloadChangedAssets() == 1 && Dependent.GetPublishedGeneration() == DependentGeneration + 1U,
			"dependency timestamp change did not invalidate and reload its owning asset");

	AdvanceValidationWriteTime(FailingPath);
	AdvanceValidationWriteTime(HealthyPath);
	FailValidationAsset.store(true, std::memory_order_relaxed);
	Require(Assets.ReloadChangedAssets() == 2, "reload pass did not process every changed root asset");
	const resource::AssetRecord *FailingRecord = Assets.GetRecord(resource::AssetType::ShaderSource, FailingPath);
	Require(FailingRecord != nullptr && FailingRecord->GetState() == resource::AssetLoadState::Failed && !FailingRecord->GetError().empty(),
			"failed reload did not publish the failed state and diagnostic");
	Require(Failing.GetPublishedGeneration() == FailingGeneration && Failing.Pin() != nullptr,
			"failed reload discarded the prior successfully loaded asset generation");
	Require(Healthy.GetPublishedGeneration() == HealthyGeneration + 1U && Healthy.Pin() != nullptr,
			"one failed reload prevented a later changed asset from reloading");
	FailValidationAsset.store(false, std::memory_order_relaxed);
}
} // namespace

void RunDeterministicRenderCoreChecks(pipeline::device::Device &Device)
{
	ValidateSceneStorageAndCommands();
	ValidateTransactionalLightUploads();
	ValidateEventListenerMutation();
	ValidateReservedAssetLoading();
	ValidateAssetReloadPipeline();
	ValidateGraphDependencies(Device);
	ValidateGraphResourceLifetimes(Device);
	ValidateScenePreparation(Device);
	ValidateGPUVisibilityCompaction(Device);
}
} // namespace renderer::validation
