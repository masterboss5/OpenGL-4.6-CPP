#include "RenderCoreValidation.h"

#include "src/pipeline/graph/RenderGraph.h"
#include "src/pipeline/graph/HybridDeferredFrameGraph.h"
#include "src/pipeline/graph/RenderPass.h"
#include "src/pipeline/render/RenderData.h"
#include "src/pipeline/render/SceneExtractor.h"
#include "src/pipeline/render/ScenePreparation.h"
#include "src/pipeline/render/SceneRenderSnapshot.h"
#include "src/pipeline/render/SelectionMask.h"
#include "src/pipeline/render/ViewportPicker.h"
#include "src/core/events/EventDispatcher.h"
#include "src/core/threading/RenderThread.h"
#include "src/core/threading/TaskScheduler.h"
#include "src/core/window/Context.h"
#include "src/editor/commands/CommandHistory.h"
#include "src/editor/asset/PrimitiveMeshFactory.h"
#include "src/editor/document/SceneDocument.h"
#include "src/editor/project/Project.h"
#include "src/editor/reflection/ReflectionRegistry.h"
#include "src/pipeline/lighting/LightBufferManager.h"
#include "src/pipeline/mesh/MeshGpuResource.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectLightComponents.h"
#include "src/pipeline/device/Device.h"
#include "src/pipeline/shader/ShaderException.h"
#include "src/pipeline/shader/ShaderLibrary.h"
#include "src/pipeline/texture/Texture2D.h"
#include "src/pipeline/vertex/VertexDescriptor.h"
#include "src/resource/asset/AssetManager.h"
#include "src/resource/asset/AnimationAsset.h"
#include "src/resource/asset/MaterialAsset.h"
#include "src/resource/asset/importer/AssetImporter.h"
#include "src/scene/Camera.h"
#include "src/scene/Scene.h"
#include "src/scene/SceneCollection.h"
#include "src/scene/SceneCommandBuffer.h"
#include "src/scene/TransformMath.h"
#include "src/scene/detail/DenseGenerationalPool.h"
#include "src/util/memory/TypedPoolAllocator.h"

#include <ext/matrix_clip_space.hpp>
#include <gtc/matrix_transform.hpp>
#include <GL/glew.h>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace pipeline::validation
{
using pipeline::lighting::LightBufferManager;
using namespace pipeline::render;

namespace
{
std::atomic<uint32> ValidationImportCount{0};
std::atomic<bool> FailValidationAsset{false};
std::atomic<uint32> ParallelValidationImports{0};
std::atomic<uint32> MaximumParallelValidationImports{0};
std::atomic<uint32> ParallelValidationArrivals{0};

class ValidationAsset final : public resource::Asset
{
  public:
	ValidationAsset() : Asset(util::UUID::GenerateRandomUUID())
	{
	}
};

class ValidationGPUAsset final : public resource::Asset
{
  public:
	inline static constexpr resource::AssetType AssetType = resource::AssetType::ShaderSource;

	ValidationGPUAsset(const uint32 Marker, const bool RequiresGPU, const bool GPURealizationSucceeds)
		: Asset(util::UUID::GenerateRandomUUID()), Marker(Marker), RequiresGPU(RequiresGPU), GPURealizationSucceeds(GPURealizationSucceeds)
	{
	}

	[[nodiscard]] bool RequiresGPURealization() const noexcept override
	{
		return this->RequiresGPU;
	}

	[[nodiscard]] resource::AssetGPURealizationResult RealizeGPU(pipeline::device::Device &) override
	{
		return {.Succeeded = this->GPURealizationSucceeds,
				.Error = this->GPURealizationSucceeds ? string{} : string("Injected deterministic GPU realization failure")};
	}

	[[nodiscard]] uint32 GetMarker() const noexcept
	{
		return this->Marker;
	}

  private:
	uint32 Marker = 0;
	bool RequiresGPU = false;
	bool GPURealizationSucceeds = false;
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
		if (Path.filename() == "ParallelA.shader" || Path.filename() == "ParallelB.shader")
		{
			const uint32 Active = ParallelValidationImports.fetch_add(1, std::memory_order_acq_rel) + 1U;
			uint32 ObservedMaximum = MaximumParallelValidationImports.load(std::memory_order_acquire);
			while (ObservedMaximum < Active &&
				   !MaximumParallelValidationImports.compare_exchange_weak(ObservedMaximum, Active, std::memory_order_acq_rel))
			{
			}
			ParallelValidationArrivals.fetch_add(1, std::memory_order_release);
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (ParallelValidationArrivals.load(std::memory_order_acquire) < 2U && std::chrono::steady_clock::now() < Deadline)
				std::this_thread::yield();
			ParallelValidationImports.fetch_sub(1, std::memory_order_acq_rel);
		}
		if (Path.filename() == "Failing.shader" && FailValidationAsset.load(std::memory_order_relaxed))
		{
			throw resource::importer::AssetContentValidationException(resource::AssetType::ShaderSource, Path,
																	  "Injected deterministic reload failure");
		}
		if (Path.filename() == "Fullscreen.vert")
			(void)Context.Reserve<ValidationAsset>(resource::AssetType::ShaderSource, Path.parent_path() / "ToneMap.frag");
		if (Path.filename() == "ReservationFailure.shader")
		{
			(void)Context.Reserve<ValidationAsset>(resource::AssetType::ShaderSource, Path.parent_path() / "NewReservation.shader");
			(void)Context.Reserve<ValidationAsset>(resource::AssetType::ShaderSource, Path.parent_path() / "ExistingReservation.shader");
			throw resource::importer::AssetContentValidationException(resource::AssetType::ShaderSource, Path,
																	  "Injected reservation transaction failure");
		}
		std::vector<resource::AssetDependency> Dependencies;
		if (Path.filename() == "Dependent.shader")
			Dependencies.push_back({resource::AssetType::ShaderSource, Path.parent_path() / "Dependency.inc"});
		else if (Path.filename() == "CycleA.shader")
			Dependencies.push_back({resource::AssetType::ShaderSource, Path.parent_path() / "CycleB.shader"});
		else if (Path.filename() == "CycleB.shader")
			Dependencies.push_back({resource::AssetType::ShaderSource, Path.parent_path() / "CycleA.shader"});
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

struct ValidationPoolValue final
{
	int32 Value = 0;
	int32 *DestructionOrder = nullptr;
	usize *DestructionCount = nullptr;

	ValidationPoolValue(const int32 Value, int32 *DestructionOrder = nullptr, usize *DestructionCount = nullptr) noexcept
		: Value(Value), DestructionOrder(DestructionOrder), DestructionCount(DestructionCount)
	{
	}

	ValidationPoolValue(ValidationPoolValue &&Other) noexcept
		: Value(Other.Value), DestructionOrder(Other.DestructionOrder), DestructionCount(Other.DestructionCount)
	{
		Other.DestructionOrder = nullptr;
		Other.DestructionCount = nullptr;
	}

	~ValidationPoolValue() noexcept
	{
		if (this->DestructionOrder != nullptr && this->DestructionCount != nullptr)
		{
			this->DestructionOrder[(*this->DestructionCount)++] = this->Value;
		}
	}
};

struct ThrowingDestructorValue final
{
	~ThrowingDestructorValue() noexcept(false)
	{
	}
};

static_assert(PoolAllocatable<ValidationPoolValue>);
static_assert(!PoolAllocatable<ThrowingDestructorValue>);

void ValidateMemoryPools()
{
	int32 DestructionOrder[4]{};
	usize DestructionCount = 0;
	world::detail::DenseGenerationalPool<ValidationPoolValue> DensePool(2);
	const world::detail::DensePoolHandle First = DensePool.Emplace(1, DestructionOrder, &DestructionCount);
	const world::detail::DensePoolHandle Second = DensePool.Emplace(2, DestructionOrder, &DestructionCount);
	DensePool.Clear();
	Require(DensePool.Size() == 0 && !DensePool.Contains(First) && !DensePool.Contains(Second),
			"dense pool clear retained size or stale handles");
	Require(DestructionCount == 2 && DestructionOrder[0] == 2 && DestructionOrder[1] == 1,
			"dense pool clear did not destroy live values exactly once in reverse order");
	const world::detail::DensePoolHandle ReusedFirst = DensePool.Emplace(3);
	const world::detail::DensePoolHandle ReusedSecond = DensePool.Emplace(4);
	Require(ReusedFirst.Slot == 0 && ReusedSecond.Slot == 1 && ReusedFirst.Generation != First.Generation &&
				ReusedSecond.Generation != Second.Generation,
			"dense pool clear did not rebuild bounded slot allocation with advanced generations");
	Require(world::detail::NextSceneGeneration(std::numeric_limits<uint32>::max()) == 1U,
			"dense pool generation rollover produced the invalid zero generation");

	DestructionCount = 0;
	memory::TypedPoolAllocator<ValidationPoolValue> TypedPool(2);
	ValidationPoolValue *TypedFirst = TypedPool.Allocate(5, DestructionOrder, &DestructionCount);
	ValidationPoolValue *TypedSecond = TypedPool.Allocate(6, DestructionOrder, &DestructionCount);
	const void *InteriorAddress = reinterpret_cast<const uint8 *>(TypedFirst) + 1U;
	Require(TypedPool.Contains(TypedFirst) && TypedPool.Contains(TypedSecond) &&
				!TypedPool.Contains(reinterpret_cast<const ValidationPoolValue *>(InteriorAddress)) &&
				TypedPool.OwnsStorageAddress(InteriorAddress),
			"typed pool did not distinguish live object starts from owned raw storage");
	const bool ForeignThreadRejected = std::async(std::launch::async,
												  [&TypedPool]()
												  {
													  try
													  {
														  (void)TypedPool.GetCount();
													  }
													  catch (const std::logic_error &)
													  {
														  return true;
													  }
													  return false;
												  })
										   .get();
	Require(ForeignThreadRejected, "typed pool accepted access from a non-owner thread");
	TypedPool.Reset();
	Require(DestructionCount == 2 && DestructionOrder[0] == 6 && DestructionOrder[1] == 5 && TypedPool.IsEmpty(),
			"typed pool reset did not destroy live values exactly once in reverse order");
}

void ValidateTransformSafety()
{
	glm::mat4 NonFiniteMatrix(1.0f);
	NonFiniteMatrix[0][0] = std::numeric_limits<float32>::quiet_NaN();
	bool NonFiniteMatrixRejected = false;
	try
	{
		(void)world::DecomposeAffineTransform(NonFiniteMatrix);
	}
	catch (const std::invalid_argument &)
	{
		NonFiniteMatrixRejected = true;
	}
	Require(NonFiniteMatrixRejected, "transform decomposition accepted a non-finite matrix");

	const float32 LargeScale = 1.0e20f;
	const world::DecomposedTransform Large = world::DecomposeAffineTransform(glm::scale(glm::mat4(1.0f), glm::vec3(LargeScale)));
	Require(world::IsFiniteTransformValue(Large.Scale) && std::abs(Large.Scale.x / LargeScale - 1.0f) < 1.0e-5f,
			"transform decomposition overflowed a finite large scale");

	components::CObjectTransformComponent Transform({});
	const uint64 InitialRevision = Transform.GetRevision();
	Transform.SetScale(glm::vec3(world::MinimumTransformScale));
	Require(Transform.GetScale() == glm::vec3(1.0f) && Transform.GetRevision() == InitialRevision,
			"transform component accepted scale outside the shared decomposition domain");
	Transform.SetRotation(glm::quat(std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max(), 0.0f, 0.0f));
	Require(world::IsFiniteTransformValue(Transform.GetRotation()) && Transform.GetRevision() == InitialRevision + 1U,
			"transform component failed to normalize a finite extreme quaternion safely");

	Transform.SetPosition(glm::vec3(std::numeric_limits<float32>::max()));
	const glm::mat4 MatrixSnapshot = Transform.GetMatrix();
	const uint64 BeforeOverflow = Transform.GetRevision();
	Transform.Translate(glm::vec3(std::numeric_limits<float32>::max()));
	Require(Transform.GetRevision() == BeforeOverflow && Transform.GetPosition() == glm::vec3(std::numeric_limits<float32>::max()),
			"transform component published an overflowed translation");
	Transform.SetPosition({1.0f, 2.0f, 3.0f});
	Require(MatrixSnapshot[3] == glm::vec4(std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max(),
										   std::numeric_limits<float32>::max(), 1.0f),
			"transform matrix query did not return an immutable value snapshot");

	const glm::quat RotationBeforeLookAt = Transform.GetRotation();
	Transform.SetPosition(glm::vec3(std::numeric_limits<float32>::max()));
	const uint64 BeforeLookAt = Transform.GetRevision();
	Transform.LookAt(glm::vec3(-std::numeric_limits<float32>::max()));
	Require(Transform.GetRevision() == BeforeLookAt && Transform.GetRotation() == RotationBeforeLookAt,
			"transform LookAt published state after derived-vector overflow");
}

void ValidateGraphDependencies(pipeline::device::Device &Device)
{
	bool RejectedInvalidPass = false;
	try
	{
		const graph::RenderPass InvalidPass(graph::RenderPassDescription{});
		(void)InvalidPass;
	}
	catch (const std::invalid_argument &)
	{
		RejectedInvalidPass = true;
	}
	Require(RejectedInvalidPass, "render-pass ownership accepted an unnamed pass without an execution callback");

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

	graph::RenderGraph InvalidAttachments(Device);
	InvalidAttachments.BeginFrame({16, 16});
	const graph::TextureHandle AttachmentColor = InvalidAttachments.CreateTexture({.DebugName = "AttachmentColor", .Extent = {16, 16}});
	const graph::TextureHandle AttachmentDepth =
		InvalidAttachments.CreateTexture({.DebugName = "AttachmentDepth", .Extent = {8, 8}, .Format = graph::TextureFormat::Depth32Float});
	(void)InvalidAttachments.AddPass(
		{.Name = "MismatchedAttachments",
		 .Queue = graph::PassQueue::Graphics,
		 .ColorAttachments = {{.Texture = AttachmentColor, .Load = graph::LoadOperation::Clear}},
		 .DepthAttachment = graph::DepthAttachment{.Texture = AttachmentDepth, .Load = graph::LoadOperation::Clear},
		 .Execute = [](graph::RenderGraphContext &) {}});
	bool RejectedMismatchedAttachments = false;
	try
	{
		InvalidAttachments.Compile();
	}
	catch (const std::logic_error &)
	{
		RejectedMismatchedAttachments = true;
	}
	Require(RejectedMismatchedAttachments, "a render pass accepted incompatible framebuffer attachment layouts");

	graph::RenderGraph UndeclaredAccessGraph(Device);
	UndeclaredAccessGraph.BeginFrame({16, 16});
	const graph::TextureHandle UndeclaredTexture =
		UndeclaredAccessGraph.CreateTexture({.DebugName = "UndeclaredTexture", .Extent = {16, 16}, .Persistent = true});
	(void)UndeclaredAccessGraph.AddPass(
		{.Name = "UndeclaredAccess",
		 .Queue = graph::PassQueue::Compute,
		 .Execute = [UndeclaredTexture](graph::RenderGraphContext &Context) { (void)Context.GetTexture(UndeclaredTexture); }});
	UndeclaredAccessGraph.Compile();
	bool RejectedUndeclaredAccess = false;
	try
	{
		UndeclaredAccessGraph.Execute();
	}
	catch (const std::logic_error &)
	{
		RejectedUndeclaredAccess = true;
	}
	Require(RejectedUndeclaredAccess, "a render-graph callback accessed a texture not declared by its pass");

	graph::RenderGraph ValidGraph(Device);
	ValidGraph.BeginFrame({16, 16});
	const graph::TextureHandle ProducedTexture = ValidGraph.CreateTexture({.DebugName = "ProducedTexture", .Extent = {16, 16}});
	graph::RenderPass ProducePass({.Name = "Produce",
								   .Queue = graph::PassQueue::Compute,
								   .WriteTextures = {ProducedTexture},
								   .Execute = [](graph::RenderGraphContext &) {}});
	(void)ValidGraph.AddPass(std::move(ProducePass));
	(void)ValidGraph.AddPass({.Name = "Consume",
							  .Queue = graph::PassQueue::Compute,
							  .ReadTextures = {ProducedTexture},
							  .Execute = [](graph::RenderGraphContext &) {}});
	ValidGraph.Compile();
	Require(ValidGraph.IsCompiled(), "a valid producer-consumer graph did not compile");
	const graph::RenderGraphInspection Inspection = ValidGraph.Inspect();
	Require(Inspection.Passes == std::vector<string>{"Produce", "Consume"} &&
				Inspection.Textures == std::vector<string>{"ProducedTexture"} && Inspection.Buffers.empty(),
			"render graph inspection did not preserve declared pass and resource names");
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

	// Clear load operations must not inherit scissor or write masks from a
	// previous graphics pass. Tone mapping leaves depth writes disabled, so
	// this specifically guards against stale depth surviving into the next frame.
	graph::RenderGraph ClearStateGraph(Device);
	ClearStateGraph.BeginFrame({4, 4});
	const graph::TextureHandle ClearedColor =
		ClearStateGraph.CreateTexture({.DebugName = "ClearStateColor", .Extent = {4, 4}, .Format = graph::TextureFormat::RGBA16Float});
	const graph::TextureHandle ClearedDepth =
		ClearStateGraph.CreateTexture({.DebugName = "ClearStateDepth", .Extent = {4, 4}, .Format = graph::TextureFormat::Depth32Float});
	constexpr glm::vec4 ExpectedClearColor{0.125f, 0.25f, 0.5f, 1.0f};
	constexpr float32 ExpectedClearDepth = 0.375f;
	(void)ClearStateGraph.AddPass(
		{.Name = "ClearStateIsolation",
		 .Queue = graph::PassQueue::Graphics,
		 .ColorAttachments = {{.Texture = ClearedColor, .Load = graph::LoadOperation::Clear, .ClearColor = ExpectedClearColor}},
		 .DepthAttachment =
			 graph::DepthAttachment{.Texture = ClearedDepth, .Load = graph::LoadOperation::Clear, .ClearDepth = ExpectedClearDepth},
		 .Execute = [](graph::RenderGraphContext &) {}});
	ClearStateGraph.ExportTexture(ClearedColor);
	ClearStateGraph.ExportTexture(ClearedDepth);
	ClearStateGraph.Compile();
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, 1, 1);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask(GL_FALSE);
	ClearStateGraph.Execute();
	const graph::ExportedTexture ColorExport = ClearStateGraph.GetExportedTexture(ClearedColor);
	const graph::ExportedTexture DepthExport = ClearStateGraph.GetExportedTexture(ClearedDepth);
	std::array<glm::vec4, 16> ColorPixels{};
	std::array<float32, 16> DepthPixels{};
	glGetTextureImage(ColorExport.Texture, 0, GL_RGBA, GL_FLOAT, static_cast<GLsizei>(sizeof(ColorPixels)), ColorPixels.data());
	glGetTextureImage(DepthExport.Texture, 0, GL_DEPTH_COMPONENT, GL_FLOAT, static_cast<GLsizei>(sizeof(DepthPixels)), DepthPixels.data());
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	Device.InvalidateGraphicsPipelineStateCache();
	Require(std::ranges::all_of(ColorPixels, [ExpectedClearColor](const glm::vec4 &Color)
								{ return glm::all(glm::lessThan(glm::abs(Color - ExpectedClearColor), glm::vec4(0.001f))); }) &&
				std::ranges::all_of(DepthPixels,
									[ExpectedClearDepth](const float32 Depth) { return std::abs(Depth - ExpectedClearDepth) < 0.0001f; }),
			"render-graph clear load operations inherited stale scissor or write-mask state");
	Device.CheckErrors("render-graph clear-state isolation validation");

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

	graph::RenderGraph PersistentGraph(Device);
	GLuint LargePersistentTexture = 0;
	PersistentGraph.BeginFrame({16, 16});
	const graph::TextureHandle LargePersistent = PersistentGraph.CreateTexture({.DebugName = "ResizablePersistent",
																				.Extent = {16, 16},
																				.Dimension = graph::TextureDimension::Texture2DArray,
																				.Layers = 4,
																				.Persistent = true});
	(void)PersistentGraph.AddPass({.Name = "WriteLargePersistent",
								   .Queue = graph::PassQueue::Compute,
								   .WriteTextures = {LargePersistent},
								   .Execute = [&LargePersistentTexture, LargePersistent](graph::RenderGraphContext &Context)
								   { LargePersistentTexture = Context.GetTexture(LargePersistent); }});
	PersistentGraph.Compile();
	PersistentGraph.Execute();
	PersistentGraph.BeginFrame({16, 16});
	GLuint SmallPersistentTexture = 0;
	const graph::TextureHandle SmallPersistent = PersistentGraph.CreateTexture({.DebugName = "ResizablePersistent",
																				.Extent = {16, 16},
																				.Dimension = graph::TextureDimension::Texture2DArray,
																				.Layers = 1,
																				.Persistent = true});
	(void)PersistentGraph.AddPass({.Name = "WriteSmallPersistent",
								   .Queue = graph::PassQueue::Compute,
								   .WriteTextures = {SmallPersistent},
								   .Execute = [&SmallPersistentTexture, SmallPersistent](graph::RenderGraphContext &Context)
								   { SmallPersistentTexture = Context.GetTexture(SmallPersistent); }});
	PersistentGraph.Compile();
	PersistentGraph.Execute();
	Require(LargePersistentTexture != 0 && LargePersistentTexture == SmallPersistentTexture,
			"persistent texture shrink allocated a duplicate physical texture instead of reusing the larger array");

	graph::RenderGraph ExportGraph(Device);
	ExportGraph.BeginFrame({16, 16});
	const graph::TextureHandle Exported = ExportGraph.CreateTexture({.DebugName = "Exported", .Extent = {16, 16}});
	const graph::TextureHandle AfterExport = ExportGraph.CreateTexture({.DebugName = "AfterExport", .Extent = {16, 16}});
	GLuint ExportedPhysicalTexture = 0;
	GLuint AfterExportPhysicalTexture = 0;
	(void)ExportGraph.AddPass({.Name = "WriteExported",
							   .Queue = graph::PassQueue::Compute,
							   .WriteTextures = {Exported},
							   .Execute = [&ExportedPhysicalTexture, Exported](graph::RenderGraphContext &Context)
							   { ExportedPhysicalTexture = Context.GetTexture(Exported); }});
	(void)ExportGraph.AddPass({.Name = "WriteAfterExport",
							   .Queue = graph::PassQueue::Compute,
							   .WriteTextures = {AfterExport},
							   .Execute = [&AfterExportPhysicalTexture, AfterExport](graph::RenderGraphContext &Context)
							   { AfterExportPhysicalTexture = Context.GetTexture(AfterExport); }});
	ExportGraph.ExportTexture(Exported);
	ExportGraph.Compile();
	ExportGraph.Execute();
	const graph::ExportedTexture Export = ExportGraph.GetExportedTexture(Exported);
	Require(Export.IsValid() && Export.Texture == ExportedPhysicalTexture && ExportedPhysicalTexture != AfterExportPhysicalTexture,
			"exported render-graph texture was aliased or did not resolve to its declared physical allocation");

	graph::RenderGraph IndependentViewGraph(Device);
	IndependentViewGraph.BeginFrame({16, 16}, 17, 3);
	const graph::TextureHandle IndependentViewTexture =
		IndependentViewGraph.CreateTexture({.DebugName = "IndependentViewExport", .Extent = {16, 16}});
	(void)IndependentViewGraph.AddPass({.Name = "WriteIndependentView",
										.Queue = graph::PassQueue::Compute,
										.WriteTextures = {IndependentViewTexture},
										.Execute = [](graph::RenderGraphContext &) {}});
	IndependentViewGraph.ExportTexture(IndependentViewTexture);
	IndependentViewGraph.Compile();
	IndependentViewGraph.Execute();
	const graph::ExportedTexture IndependentViewExport = IndependentViewGraph.GetExportedTexture(IndependentViewTexture);
	Require(IndependentViewExport.IsValid() && IndependentViewExport.Texture != Export.Texture &&
				IndependentViewExport.ViewIdentity == 17 && IndependentViewExport.ViewGeneration == 3,
			"independent render-view graphs aliased exported texture ownership");

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
	// receives a distinct allocation. Renderer compares this extent
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

void ValidateMaximumShadowBudgetAllocation(pipeline::device::Device &Device)
{
	GLuint ImportedBuffer = 0;
	glCreateBuffers(1, &ImportedBuffer);
	glNamedBufferStorage(ImportedBuffer, 256, nullptr, GL_DYNAMIC_STORAGE_BIT);
	struct BufferCleanup final
	{
		GLuint &Buffer;
		~BufferCleanup()
		{
			if (this->Buffer != 0)
				glDeleteBuffers(1, &this->Buffer);
		}
	};
	[[maybe_unused]] const BufferCleanup Cleanup{ImportedBuffer};
	graph::RenderGraph Graph(Device);
	Graph.BeginFrame({32, 32}, 1, 1);
	const auto Import = [&Graph, ImportedBuffer](const string_view Name)
	{ return Graph.ImportBuffer(Name, 256, GL_DYNAMIC_STORAGE_BIT, ImportedBuffer); };
	const graph::HybridDeferredFrameInputs Inputs{.Extent = {32, 32},
												  .CandidateInstances = Import("Candidates"),
												  .ShadowInstances = Import("ShadowInstances"),
												  .VisibleInstances = Import("VisibleInstances"),
												  .IndirectCommands = Import("IndirectCommands"),
												  .BatchMetadata = Import("BatchMetadata"),
												  .VisibilityScratch = Import("VisibilityScratch"),
												  .SelectionMask = Import("SelectionMask"),
												  .SelectionMaskWordCount = 1,
												  .DirectionalShadowLayerCount = pipeline::render::DirectionalShadowCascadeCount,
												  .SpotShadowLayerCount = pipeline::render::MaximumSpotShadowCount,
												  .PointShadowFaceLayerCount = pipeline::render::MaximumPointShadowFaceCount,
												  .TemporalHistoryWriteIndex = 0,
												  .HistoryNamespace = "MaximumShadowBudget"};
	uint8 CallbackOwner = 0;
	const auto NoOpExecutor = [](void *, graph::HybridDeferredPassID, graph::RenderGraphContext &,
								 const graph::HybridDeferredFrameResources &) {};
	const auto Callback = [&](const graph::HybridDeferredPassID Pass)
	{ return graph::HybridDeferredPassCallback{.Executor = NoOpExecutor, .UserData = &CallbackOwner, .Pass = Pass}; };
	const graph::HybridDeferredPassCallbacks Callbacks{.DirectionalShadows = Callback(graph::HybridDeferredPassID::DirectionalShadows),
													   .SpotShadows = Callback(graph::HybridDeferredPassID::SpotShadows),
													   .PointShadows = Callback(graph::HybridDeferredPassID::PointShadows),
													   .MainVisibility = Callback(graph::HybridDeferredPassID::MainVisibility),
													   .AccuratePicking = Callback(graph::HybridDeferredPassID::AccuratePicking),
													   .DepthPrepass = Callback(graph::HybridDeferredPassID::DepthPrepass),
													   .HierarchicalDepth = Callback(graph::HybridDeferredPassID::HierarchicalDepth),
													   .GBuffer = Callback(graph::HybridDeferredPassID::GBuffer),
													   .ClusteredLights = Callback(graph::HybridDeferredPassID::ClusteredLights),
													   .DeferredLighting = Callback(graph::HybridDeferredPassID::DeferredLighting),
													   .WeightedOIT = Callback(graph::HybridDeferredPassID::WeightedOIT),
													   .OITComposition = Callback(graph::HybridDeferredPassID::OITComposition),
													   .TemporalAA = Callback(graph::HybridDeferredPassID::TemporalAA),
													   .ViewportVisualization =
														   Callback(graph::HybridDeferredPassID::ViewportVisualization),
													   .SelectionOutline = Callback(graph::HybridDeferredPassID::SelectionOutline),
													   .WireframeOverlay = Callback(graph::HybridDeferredPassID::WireframeOverlay),
													   .EditorGrid = Callback(graph::HybridDeferredPassID::EditorGrid),
													   .DebugOverlay = Callback(graph::HybridDeferredPassID::DebugOverlay),
													   .GizmoOverlay = Callback(graph::HybridDeferredPassID::GizmoOverlay),
													   .ExposureAndBloom = Callback(graph::HybridDeferredPassID::ExposureAndBloom),
													   .ToneMapAndPresent = Callback(graph::HybridDeferredPassID::ToneMapAndPresent)};
	graph::HybridDeferredFrameGraph Builder;
	graph::HybridDeferredFrameResources Resources;
	Builder.BuildInto(Graph, Inputs, Callbacks, Resources);
	Graph.Compile();
	Require(Graph.IsCompiled(), "maximum bounded shadow arrays could not be allocated within the renderer budget");
	Device.CheckErrors("maximum bounded shadow-array allocation validation");
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
		std::cerr << "[Validation] GPU visibility pipelines compiled\n";

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
		Device.CheckErrors("GPU visibility frame buffer creation");
		std::cerr << "[Validation] GPU visibility frame buffer created\n";
		glCreateBuffers(1, &CandidateBuffer);
		glNamedBufferStorage(CandidateBuffer, static_cast<GLsizeiptr>(Candidates.size() * sizeof(PreparedInstance)), Candidates.data(), 0);
		Device.CheckErrors("GPU visibility candidate buffer creation");
		std::cerr << "[Validation] GPU visibility candidate buffer created\n";
		glCreateBuffers(1, &VisibleBuffer);
		glNamedBufferStorage(VisibleBuffer, static_cast<GLsizeiptr>(Candidates.size() * sizeof(PreparedInstance)), nullptr,
							 GL_DYNAMIC_STORAGE_BIT);
		Device.CheckErrors("GPU visibility visible buffer creation");
		std::cerr << "[Validation] GPU visibility visible buffer created\n";
		glCreateBuffers(1, &CommandBuffer);
		glNamedBufferStorage(CommandBuffer, static_cast<GLsizeiptr>(Commands.size() * sizeof(RenderCommand)), Commands.data(),
							 GL_DYNAMIC_STORAGE_BIT);
		Device.CheckErrors("GPU visibility command buffer creation");
		std::cerr << "[Validation] GPU visibility command buffer created\n";
		glCreateBuffers(1, &ScratchBuffer);
		glNamedBufferStorage(ScratchBuffer, static_cast<GLsizeiptr>((ScratchCapacity * 4U + 512U) * sizeof(uint32)), nullptr,
							 GL_DYNAMIC_STORAGE_BIT);
		Device.CheckErrors("GPU visibility scratch buffer creation");
		std::cerr << "[Validation] GPU visibility scratch buffer created\n";
		glCreateTextures(GL_TEXTURE_2D, 1, &HierarchyTexture);
		glTextureStorage2D(HierarchyTexture, 1, GL_R32F, 1, 1);
		Device.CheckErrors("GPU visibility hierarchy texture creation");
		std::cerr << "[Validation] GPU visibility resources created\n";

		const uint32 Zero = 0;
		glClearNamedBufferData(ScratchBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &Zero);
		glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(RendererBinding::FrameConstants), FrameBuffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::Candidates), CandidateBuffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::Instances), VisibleBuffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::VisibilityScratch), ScratchBuffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::IndirectCommands), CommandBuffer);
		glBindTextureUnit(0, HierarchyTexture);

		auto &Cull = Shaders.GetComputePipeline(CullPipeline);
		Cull.SetUniformUInt(pipeline::shader::ComputeUniform::CandidateCount, CandidateCount);
		Cull.SetUniformUInt(pipeline::shader::ComputeUniform::PyramidMipCount, 1);
		Cull.SetUniformUInt(pipeline::shader::ComputeUniform::HistoryValid, 0);
		Cull.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, ScratchCapacity);
		Cull.Bind();
		glDispatchCompute((CandidateCount + 63U) / 64U, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
		std::cerr << "[Validation] GPU visibility classification complete\n";

		auto &Prefix = Shaders.GetComputePipeline(PrefixPipeline);
		Prefix.SetUniformUInt(pipeline::shader::ComputeUniform::BatchCount, CandidateCount);
		Prefix.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, ScratchCapacity);
		Prefix.Bind();
		glDispatchCompute(ScanBlockCount, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		std::cerr << "[Validation] GPU visibility local prefix complete\n";

		auto &BlockPrefix = Shaders.GetComputePipeline(BlockPrefixPipeline);
		BlockPrefix.SetUniformUInt(pipeline::shader::ComputeUniform::BlockCount, ScanBlockCount);
		BlockPrefix.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, ScratchCapacity);
		BlockPrefix.Bind();
		glDispatchCompute(1, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		std::cerr << "[Validation] GPU visibility block prefix complete\n";

		auto &Finalize = Shaders.GetComputePipeline(FinalizePipeline);
		Finalize.SetUniformUInt(pipeline::shader::ComputeUniform::BatchCount, CandidateCount);
		Finalize.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, ScratchCapacity);
		Finalize.Bind();
		glDispatchCompute(ScanBlockCount, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		std::cerr << "[Validation] GPU visibility finalize complete\n";

		auto &Scatter = Shaders.GetComputePipeline(ScatterPipeline);
		Scatter.SetUniformUInt(pipeline::shader::ComputeUniform::CandidateCount, CandidateCount);
		Scatter.SetUniformUInt(pipeline::shader::ComputeUniform::ScratchCapacity, ScratchCapacity);
		Scatter.Bind();
		glDispatchCompute((CandidateCount + 63U) / 64U, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
		Device.CheckErrors("deterministic GPU visibility validation");
		std::cerr << "[Validation] GPU visibility scatter complete\n";

		std::vector<RenderCommand> CompactCommands(CandidateCount);
		std::vector<PreparedInstance> CompactInstances(CandidateCount);
		glGetNamedBufferSubData(CommandBuffer, 0, static_cast<GLsizeiptr>(CompactCommands.size() * sizeof(RenderCommand)),
								CompactCommands.data());
		glGetNamedBufferSubData(VisibleBuffer, 0, static_cast<GLsizeiptr>(CompactInstances.size() * sizeof(PreparedInstance)),
								CompactInstances.data());
		Device.CheckErrors("GPU visibility validation readback");
		std::cerr << "[Validation] GPU visibility readback complete\n";
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

void ValidateShaderResourceContracts(pipeline::device::Device &Device)
{
	resource::AssetManager Assets;
	pipeline::shader::ShaderLibrary Shaders(Device, Assets);
	const auto ExpectInterfaceFailure = [&Shaders](pipeline::shader::ShaderResourceContract Contract, const string_view Label)
	{
		bool Rejected = false;
		try
		{
			(void)Shaders.CreateComputePipeline(
				{.Compute = {.Path = "shader/Visibility.comp", .Stage = pipeline::shader::ShaderStage::Compute},
				 .RequiredResources = {std::move(Contract)}});
		}
		catch (const pipeline::shader::ShaderInterfaceException &)
		{
			Rejected = true;
		}
		Require(Rejected, string(Label) + " shader resource contract was not rejected");
	};

	const uint32 ValidPipeline =
		Shaders.CreateComputePipeline({.Compute = {.Path = "shader/Visibility.comp", .Stage = pipeline::shader::ShaderStage::Compute},
									   .RequiredUniforms = pipeline::shader::UniformBit(pipeline::shader::ComputeUniform::CandidateCount),
									   .RequiredResources = {{.Name = "previousHierarchicalDepth",
															  .ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
															  .Type = GL_SAMPLER_2D,
															  .Binding = 0}}});
	auto &Pipeline = Shaders.GetComputePipeline(ValidPipeline);
	const uint64 QueryCountBeforeDrawUse = pipeline::shader::ShaderModule::GetInterfaceQueryCountForValidation();
	Pipeline.Bind();
	Pipeline.SetUniformUInt(pipeline::shader::ComputeUniform::CandidateCount, 0);
	Require(pipeline::shader::ShaderModule::GetInterfaceQueryCountForValidation() == QueryCountBeforeDrawUse,
			"binding or updating a realized pipeline repeated an OpenGL interface query");

	ExpectInterfaceFailure({.Name = "previousHierarchicalDepth",
							.ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
							.Type = GL_SAMPLER_2D_ARRAY,
							.Binding = 0},
						   "sampler type mismatch");
	ExpectInterfaceFailure({.Name = "previousHierarchicalDepth",
							.ResourceClass = pipeline::shader::ShaderResourceClass::Sampler,
							.Type = GL_SAMPLER_2D,
							.Binding = 1},
						   "sampler binding mismatch");
	ExpectInterfaceFailure({.Name = "previousHierarchicalDepth",
							.ResourceClass = pipeline::shader::ShaderResourceClass::Image,
							.Type = GL_SAMPLER_2D,
							.Binding = 0},
						   "image class mismatch");
}

void ValidateScenePreparation(pipeline::device::Device &Device)
{
	(void)Device;
	const pipeline::vertex::VertexDescriptor Descriptor({{.BindingIndex = 0, .StrideInBytes = sizeof(float32) * 3U}},
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

	const pipeline::vertex::VertexDescriptor AlternateDescriptor(
		{{.BindingIndex = 0, .StrideInBytes = sizeof(float32) * 4U}},
		{{.Semantic = "POSITION", .Location = 0, .BindingIndex = 0, .ComponentCount = 3}});
	SceneCollection LayoutScene;
	LayoutScene.BeginFrame(3);
	LayoutScene.Submit(VisibleItem);
	RenderItem AlternateLayoutItem = VisibleItem;
	AlternateLayoutItem.VertexDescriptor = &AlternateDescriptor;
	AlternateLayoutItem.ObjectID = 21;
	LayoutScene.Submit(AlternateLayoutItem);
	LayoutScene.Seal();
	const RenderPreparationResult Layouts = Preparation.Prepare(LayoutScene, Projection, 0, 1);
	Require(Layouts.Batches.size() == 2 && Layouts.CandidateCommands.size() == 2,
			"different vertex descriptors were merged into one indirect batch");

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

	const world::ObjectHandle Parent = Scene.CreateObject();
	const world::ObjectHandle Child = Scene.CreateObject();
	const util::UUID ParentID = util::UUID::GenerateRandomUUID();
	(void)Scene.AddComponent<components::CObjectIdentityComponent>(Parent, "Parent", ParentID);
	(void)Scene.AddComponent<components::CObjectTransformComponent>(Parent);
	(void)Scene.AddComponent<components::CObjectHierarchyComponent>(Parent);
	(void)Scene.AddComponent<components::CObjectIdentityComponent>(Child, "Child");
	(void)Scene.AddComponent<components::CObjectTransformComponent>(Child);
	(void)Scene.AddComponent<components::CObjectHierarchyComponent>(Child);
	Scene.SetParent(Child, Parent, 3);
	{
		auto Access = Scene.Read();
		const auto ChildHierarchy = Access.GetComponent<components::CObjectHierarchyComponent>(Child);
		Require(Access.Resolve(ChildHierarchy).GetParent() == Parent && Access.Resolve(ChildHierarchy).GetSiblingOrder() == 3,
				"scene hierarchy did not publish the requested parent and sibling order");
	}
	bool CycleRejected = false;
	try
	{
		Scene.SetParent(Parent, Child);
	}
	catch (const world::SceneException &)
	{
		CycleRejected = true;
	}
	Require(CycleRejected, "scene hierarchy accepted a parent cycle");
	Require(Scene.FindObject(ParentID) == Parent, "persistent object identity lookup did not resolve the owning object");
	Require(Scene.GetObjects().size() == Scene.GetObjectCount(), "scene object enumeration did not match dense storage");
}

class IntegerEditorCommand final : public editor::commands::EditorCommand
{
  public:
	IntegerEditorCommand(int32 &Target, const int32 Value) : Target(&Target), Before(Target), After(Value)
	{
	}

	[[nodiscard]] string_view GetName() const noexcept override
	{
		return "Set Integer";
	}

	void Execute() override
	{
		*this->Target = this->After;
	}

	void Undo() override
	{
		*this->Target = this->Before;
	}

  private:
	int32 *Target = nullptr;
	int32 Before = 0;
	int32 After = 0;
};

void ValidateEditorCommandsAndReflection()
{
	int32 Value = 1;
	editor::commands::CommandHistory History(8);
	History.BeginTransaction("Integer edit");
	History.Execute(std::make_unique<IntegerEditorCommand>(Value, 2));
	History.Execute(std::make_unique<IntegerEditorCommand>(Value, 3));
	History.CommitTransaction();
	Require(Value == 3 && History.CanUndo(), "editor command transaction did not publish executed edits");
	History.Undo();
	Require(Value == 1 && History.CanRedo(), "editor command transaction did not undo atomically");
	History.Redo();
	Require(Value == 3, "editor command transaction did not redo atomically");

	struct ReflectedValue final
	{
		int32 Number = 7;
	};
	editor::reflection::ReflectionRegistry Registry;
	editor::reflection::TypeDescriptor Descriptor;
	Descriptor.Name = "validation.ReflectedValue";
	Descriptor.DisplayName = "Reflected Value";
	Descriptor.Properties.push_back(editor::reflection::MakeMemberProperty(
		"Number", "Number", "Validation", editor::reflection::PropertyKind::SignedInteger, &ReflectedValue::Number));
	const editor::reflection::ReflectionTypeID TypeID = editor::reflection::ReflectionRegistry::MakeTypeID(Descriptor.Name);
	Registry.Register(std::move(Descriptor));
	const std::optional<editor::reflection::TypeDescriptor> Registered = Registry.Find(TypeID);
	Require(Registered.has_value() && Registered->Properties.size() == 1, "reflection registry did not retain registered metadata");
	ReflectedValue Instance;
	const editor::reflection::PropertyDescriptor &Property = Registered->Properties.front();
	Require(std::get<int32>(Property.Read(&Instance)) == 7, "reflected property getter returned the wrong value");
	Property.Write(&Instance, editor::reflection::PropertyValue(int32{11}), {});
	Require(Instance.Number == 11, "reflected property setter did not mutate the target value");
}

void ValidateProjectAndSceneDocument()
{
	TemporaryValidationDirectory Temporary;
	const std::filesystem::path DescriptorPath = Temporary.GetPath() / "Validation.engineproject";
	WriteValidationFile(DescriptorPath, "{}");
	editor::project::Project Project({.Name = "Validation", .DescriptorPath = DescriptorPath});
	Project.CreateMissingDirectories();
	Project.ValidateLayout();
	Require(Project.GetAssetManager().GetRootPath() == resource::AssetManager::CanonicalizePath(Project.GetPaths().Content),
			"project asset manager was not rooted in the Content directory");
	const std::filesystem::path AssetPath = Project.ResolveContentPath("Models/Test.gltf");
	Require(Project.MakeContentRelative(AssetPath) == std::filesystem::path("Models/Test.gltf"),
			"project content path did not round-trip through its relative representation");

	editor::document::SceneDocument Document("Validation Scene", {.Objects = 16, .ComponentsPerType = 16});
	const world::ObjectHandle Parent = Document.CreateObject("Parent");
	const world::ObjectHandle Child = Document.CreateObject("Child", Parent);
	Require(Document.GetScene().GetObjectCount() == 2 && Document.IsDirty(), "scene document did not publish created objects");
	const auto ChildIdentity = Document.GetScene().GetComponent<components::CObjectIdentityComponent>(Child);
	util::UUID ChildID;
	{
		auto Access = Document.GetScene().Read();
		ChildID = Access.Resolve(ChildIdentity).GetPersistentID();
	}
	Require(Document.GetSelection().GetPrimary() == ChildID, "scene document selection did not follow object creation");
	Document.DestroyObject(ChildID);
	Require(Document.GetScene().GetObjectCount() == 1 && Document.GetSelection().Empty(),
			"scene document destruction did not prune persistent selection");
}

void ValidateTransactionalLightUploads()
{
	Require(pipeline::render::CalculateSpotShadowResolution(1U) == pipeline::render::SpotShadowResolution &&
				pipeline::render::CalculateSpotShadowResolution(pipeline::render::MaximumSpotShadowCount) == 512U &&
				pipeline::render::CalculatePointShadowResolution(6U) == pipeline::render::PointShadowResolution &&
				pipeline::render::CalculatePointShadowResolution(pipeline::render::MaximumPointShadowFaceCount) == 256U,
			"shadow resolution did not scale deterministically with the active layer count");
	const uint64 MaximumSpotShadowBytes =
		static_cast<uint64>(pipeline::render::CalculateSpotShadowResolution(pipeline::render::MaximumSpotShadowCount)) *
		static_cast<uint64>(pipeline::render::CalculateSpotShadowResolution(pipeline::render::MaximumSpotShadowCount)) *
		pipeline::render::MaximumSpotShadowCount * sizeof(float32);
	const uint64 MaximumPointShadowBytes =
		static_cast<uint64>(pipeline::render::CalculatePointShadowResolution(pipeline::render::MaximumPointShadowFaceCount)) *
		static_cast<uint64>(pipeline::render::CalculatePointShadowResolution(pipeline::render::MaximumPointShadowFaceCount)) *
		pipeline::render::MaximumPointShadowFaceCount * sizeof(float32);
	Require(MaximumSpotShadowBytes <= pipeline::render::SpotShadowMemoryBudgetBytes &&
				MaximumPointShadowBytes <= pipeline::render::PointShadowMemoryBudgetBytes,
			"maximum shadow-array allocation exceeds its per-view memory budget");

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

	LightBufferManager ShadowLights(pipeline::render::MaximumLightCount);
	std::vector<PointLightSource> ShadowPoints;
	ShadowPoints.reserve(pipeline::render::MaximumPointShadowCount + 2U);
	ShadowPoints.emplace_back(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f), 1.0f, 0.1f, 0.01f, false);
	for (uint32 Index = 0; Index < pipeline::render::MaximumPointShadowCount + 1U; ++Index)
	{
		ShadowPoints.emplace_back(glm::vec3(static_cast<float32>(Index + 1U), 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f),
								  glm::vec3(1.0f), 1.0f, 0.1f, 0.01f, true);
	}
	ShadowLights.UploadLightSources(ShadowPoints);
	const std::span<const pipeline::render::GPULightRecord> PointRecords = ShadowLights.GetGPURecords();
	Require(PointRecords.front().SpotAnglesAndShadow.w < 0.0f && PointRecords[1].SpotAnglesAndShadow.w == 0.0f &&
				PointRecords[pipeline::render::MaximumPointShadowCount].SpotAnglesAndShadow.w ==
					static_cast<float32>(pipeline::render::MaximumPointShadowCount - 1U) &&
				PointRecords.back().SpotAnglesAndShadow.w < 0.0f,
			"point-shadow allocation was not compact, bounded, and deterministic");

	const std::vector<DirectionalLightSource> Directional{
		DirectionalLightSource(glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f), false),
		DirectionalLightSource(glm::vec3(1.0f, -1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f), true)};
	ShadowLights.UploadLightSources(Directional);
	const std::span<const pipeline::render::GPULightRecord> UnifiedRecords = ShadowLights.GetGPURecords();
	Require(UnifiedRecords[0].SpotAnglesAndShadow.w < 0.0f && UnifiedRecords[1].SpotAnglesAndShadow.w == 0.0f,
			"directional-shadow allocation ignored the first eligible casting light");

	bool InvalidDirectionRejected = false;
	try
	{
		ShadowLights.UploadLightSources(std::vector<DirectionalLightSource>{
			DirectionalLightSource(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f), true)});
	}
	catch (const std::invalid_argument &)
	{
		InvalidDirectionRejected = true;
	}
	Require(InvalidDirectionRejected && ShadowLights.GetDirectionalLights().size() == Directional.size(),
			"invalid directional-light input was accepted or corrupted the published light set");
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

void ValidateTaskScheduler()
{
	core::threading::TaskScheduler Scheduler({.WorkerCount = 4, .Capacity = 256});
	std::atomic<uint32> Completed = 0;
	std::vector<std::future<void>> Futures;
	Futures.reserve(128);
	for (uint32 Index = 0; Index < 128; ++Index)
	{
		Futures.push_back(Scheduler.Submit([&Completed]() { Completed.fetch_add(1, std::memory_order_relaxed); }));
	}
	for (std::future<void> &Future : Futures)
		Future.get();
	Scheduler.WaitIdle();
	Require(Completed.load(std::memory_order_acquire) == 128, "work-stealing scheduler lost submitted work");

	core::threading::TaskGroup FailureGroup;
	FailureGroup.Run(Scheduler, []() { throw std::runtime_error("injected task-group failure"); });
	bool FailurePropagated = false;
	try
	{
		FailureGroup.Wait();
	}
	catch (const std::runtime_error &)
	{
		FailurePropagated = true;
	}
	Require(FailurePropagated, "task-group exception was not propagated to its synchronization boundary");

	core::threading::TaskGroup SelfWaitGroup;
	std::atomic<bool> SelfWaitRejected = false;
	SelfWaitGroup.Run(Scheduler,
					  [&SelfWaitGroup, &SelfWaitRejected]()
					  {
						  try
						  {
							  SelfWaitGroup.Wait();
						  }
						  catch (const core::threading::TaskSchedulerWaitException &)
						  {
							  SelfWaitRejected.store(true, std::memory_order_release);
						  }
					  });
	SelfWaitGroup.Wait();
	Require(SelfWaitRejected.load(std::memory_order_acquire), "task-group self-wait was not rejected deterministically");

	const bool WorkerIdleWaitRejected = Scheduler
											.Submit(
												[&Scheduler]()
												{
													try
													{
														Scheduler.WaitIdle();
													}
													catch (const core::threading::TaskSchedulerWaitException &)
													{
														return true;
													}
													return false;
												})
											.get();
	Require(WorkerIdleWaitRejected, "scheduler-wide worker wait was not rejected deterministically");
	bool RejectedOwnerScratchAccess = false;
	try
	{
		(void)Scheduler.GetCurrentWorkerScratch();
	}
	catch (const std::logic_error &)
	{
		RejectedOwnerScratchAccess = true;
	}
	Require(RejectedOwnerScratchAccess, "task scratch storage was exposed outside a scheduler worker");
	const bool ScratchAllocated = Scheduler
									  .Submit(
										  [&Scheduler]()
										  {
											  core::threading::TaskScratchAllocator &Scratch = Scheduler.GetCurrentWorkerScratch();
											  return Scratch.Allocate(128, alignof(uint64)) != nullptr && Scratch.GetUsed() >= 128;
										  })
									  .get();
	Require(ScratchAllocated, "scheduler worker scratch allocation failed");
	const usize ResetUsed = Scheduler.Submit([&Scheduler]() { return Scheduler.GetCurrentWorkerScratch().GetUsed(); }).get();
	Require(ResetUsed == 0, "scheduler worker scratch storage was not reset between tasks");
}

void ValidateRenderThreadContextTransfer(pipeline::device::Device &Device)
{
	core::Context &Context = Device.RequireCurrentContext();
	core::threading::RenderThread RenderThread({.QueueCapacity = 32});
	Require(!RenderThread.TryEnqueue([]() {}), "render thread accepted a command before context adoption completed");
	RenderThread.Start(Context);
	const uint32 MajorVersion = RenderThread
									.Submit(
										[&Device]()
										{
											(void)Device.RequireCurrentContext();
											GLint Version = 0;
											glGetIntegerv(GL_MAJOR_VERSION, &Version);
											return static_cast<uint32>(Version);
										})
									.get();
	std::atomic<uint32> CompletedCommands = 0;
	std::vector<std::future<void>> CommandFutures;
	CommandFutures.reserve(128);
	for (uint32 Index = 0; Index < 128; ++Index)
		CommandFutures.push_back(
			RenderThread.Submit([&CompletedCommands]() { CompletedCommands.fetch_add(1, std::memory_order_relaxed); }));
	for (std::future<void> &Future : CommandFutures)
		Future.get();
	Require(CompletedCommands.load(std::memory_order_acquire) == 128, "render-thread command ring lost or reordered queued work");
	const bool SelfStopRejected = RenderThread
									  .Submit(
										  [&RenderThread]()
										  {
											  try
											  {
												  RenderThread.Stop();
											  }
											  catch (const std::logic_error &)
											  {
												  return true;
											  }
											  return false;
										  })
									  .get();
	Require(SelfStopRejected, "render thread did not reject a self-stop/self-join attempt");
	RenderThread.Stop();
	(void)Device.RequireCurrentContext();
	Require(MajorVersion >= 4, "render thread did not receive a usable OpenGL context");

	core::threading::RenderThread FailureThread({.QueueCapacity = 8});
	FailureThread.Start(Context);
	const std::shared_ptr<std::promise<void>> FailureGate = std::make_shared<std::promise<void>>();
	const std::shared_future<void> FailureGateFuture = FailureGate->get_future().share();
	Require(FailureThread.TryEnqueue(
				[Gate = FailureGateFuture]()
				{
					Gate.wait();
					throw std::runtime_error("injected render-thread command failure");
				}),
			"render-thread failure fixture could not queue its blocking command");
	std::atomic<bool> FailureCallbackCalled = false;
	std::atomic<usize> FailureCallbackQueueCount = 0;
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		bool Queued = false;
		for (uint32 Attempt = 0; Attempt < 1'024U && !Queued; ++Attempt)
		{
			Queued = FailureThread.TryEnqueue([]() {},
											  [&FailureThread, &FailureCallbackCalled, &FailureCallbackQueueCount](const std::exception_ptr)
											  {
												  FailureCallbackQueueCount.store(FailureThread.GetQueuedCommandCount(),
																				  std::memory_order_release);
												  FailureCallbackCalled.store(true, std::memory_order_release);
											  });
			if (!Queued)
				std::this_thread::yield();
		}
		Require(Queued, "render-thread failure fixture could not queue its failure callback");
	}
	FailureGate->set_value();
	bool FailurePropagated = false;
	try
	{
		FailureThread.Stop();
	}
	catch (const std::exception &)
	{
		FailurePropagated = true;
	}
	Require(FailurePropagated && FailureCallbackCalled.load(std::memory_order_acquire) &&
				FailureCallbackQueueCount.load(std::memory_order_acquire) == 0,
			"render-thread failure callbacks did not run outside the command-ring mutex");
}

void ValidateViewportPicker(pipeline::device::Device &Device)
{
	const std::array<uint8, 4> TruncatedPixels{};
	bool RejectedTruncatedTexture = false;
	try
	{
		pipeline::texture::Texture2DCreationInfo InvalidTexture{.Pixels = TruncatedPixels.data(),
																.PixelBytes = TruncatedPixels.size(),
																.Width = 2,
																.Height = 2,
																.Channels = 4,
																.Specification =
																	pipeline::texture::Texture2DSpecification::DefaultInstance};
		pipeline::texture::Texture2D Texture(Device, "TruncatedTextureValidation", InvalidTexture);
	}
	catch (const pipeline::texture::Texture2DError &)
	{
		RejectedTruncatedTexture = true;
	}
	Require(RejectedTruncatedTexture, "Texture2D accepted a pixel source smaller than its declared upload footprint");

	pipeline::render::FramePickTable PickTable(9);
	const world::ObjectHandle FirstObject{.Scene = 7, .Slot = 0, .Generation = 1};
	const world::ObjectHandle RecycledObject{.Scene = 7, .Slot = 0, .Generation = 2};
	const pipeline::render::PickID FirstPick = PickTable.Register(FirstObject);
	Require(FirstPick != pipeline::render::BackgroundPickID && PickTable.Register(FirstObject) == FirstPick,
			"frame pick table did not preserve a nonzero stable identifier");
	const pipeline::render::PickID RecycledPick = PickTable.Register(RecycledObject);
	Require(RecycledPick != FirstPick, "frame pick table collapsed distinct object generations");
	Require(!PickTable.Resolve(pipeline::render::BackgroundPickID).has_value() && PickTable.Resolve(FirstPick) == FirstObject &&
				PickTable.Resolve(RecycledPick) == RecycledObject,
			"frame pick table did not resolve complete generation-aware object handles");
	Require(PickTable.Find(FirstObject) == FirstPick && PickTable.Find(RecycledObject) == RecycledPick,
			"frame pick table did not reverse-resolve complete object handles");
	pipeline::render::FramePickTable BoundedPickTable(10, 1);
	(void)BoundedPickTable.Register(FirstObject);
	bool RejectedPickOverflow = false;
	try
	{
		(void)BoundedPickTable.Register(RecycledObject);
	}
	catch (const std::overflow_error &)
	{
		RejectedPickOverflow = true;
	}
	Require(RejectedPickOverflow, "frame pick table grew beyond its configured capacity");
	SceneCollection BoundedCollection({.RenderItems = 1,
									   .SkinningMatrices = 1,
									   .MorphWeights = 1,
									   .PickObjects = 1,
									   .DirectionalLights = 1,
									   .PointLights = 1,
									   .SpotLights = 1});
	BoundedCollection.BeginFrame(11);
	std::shared_ptr<const pipeline::render::FramePickTable> FirstPooledTable = BoundedCollection.GetPickTable();
	BoundedCollection.BeginFrame(12);
	std::shared_ptr<const pipeline::render::FramePickTable> SecondPooledTable = BoundedCollection.GetPickTable();
	Require(FirstPooledTable != SecondPooledTable, "in-flight pick-table consumers were overwritten by the pool");
	const pipeline::render::FramePickTable *RecycledTable = FirstPooledTable.get();
	FirstPooledTable.reset();
	BoundedCollection.BeginFrame(13);
	Require(BoundedCollection.GetPickTable().get() == RecycledTable && BoundedCollection.GetPickTable()->GetFrameNumber() == 13,
			"released pick-table storage was not recycled with a new frame identity");
	SceneCollection ExhaustedCollection({.RenderItems = 1,
										 .SkinningMatrices = 1,
										 .MorphWeights = 1,
										 .PickObjects = 1,
										 .DirectionalLights = 1,
										 .PointLights = 1,
										 .SpotLights = 1});
	ExhaustedCollection.BeginFrame(21);
	const std::shared_ptr<const pipeline::render::FramePickTable> HeldTableA = ExhaustedCollection.GetPickTable();
	ExhaustedCollection.BeginFrame(22);
	const std::shared_ptr<const pipeline::render::FramePickTable> HeldTableB = ExhaustedCollection.GetPickTable();
	ExhaustedCollection.BeginFrame(23);
	const std::shared_ptr<const pipeline::render::FramePickTable> HeldTableC = ExhaustedCollection.GetPickTable();
	bool RejectedRingExhaustion = false;
	try
	{
		ExhaustedCollection.BeginFrame(24);
	}
	catch (const std::overflow_error &)
	{
		RejectedRingExhaustion = true;
	}
	Require(RejectedRingExhaustion && HeldTableA != nullptr && HeldTableB != nullptr && HeldTableC != nullptr,
			"scene collection allocated beyond its bounded in-flight pick-table ring");
	const std::array SelectedObjects{RecycledObject, world::ObjectHandle{.Scene = 7, .Slot = 99, .Generation = 1}};
	const pipeline::render::SelectionMask Selection = pipeline::render::SelectionMask::Build(PickTable, SelectedObjects);
	Require(!Selection.Contains(pipeline::render::BackgroundPickID) && !Selection.Contains(FirstPick) && Selection.Contains(RecycledPick),
			"selection bit mask did not encode only renderable selected object generations");

	GLuint Texture = 0;
	glCreateTextures(GL_TEXTURE_2D, 1, &Texture);
	glTextureStorage2D(Texture, 1, GL_R32UI, 2, 2);
	glClearTexImage(Texture, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &FirstPick);

	pipeline::render::ViewportPicker Picker(Device);
	Require(Picker.GetAvailableRequestCount() == pipeline::render::ViewportPicker::ReadbackSlotCount,
			"viewport picker did not expose its complete initial nonblocking request capacity");
	for (uint64 Request = 1; Request <= pipeline::render::ViewportPicker::ReadbackSlotCount; ++Request)
	{
		Require(
			Picker.TryRequest(Texture, {2, 2}, static_cast<uint32>(Request & 1U), static_cast<uint32>((Request >> 1U) & 1U), Request, 9),
			"viewport picker rejected an available readback slot");
	}
	Require(Picker.GetAvailableRequestCount() == 0, "viewport picker exposed an in-flight readback slot as available");
	Require(!Picker.TryRequest(Texture, {2, 2}, 0, 0, 99, 9), "viewport picker overwrote an in-flight readback slot");
	glFinish();
	const std::vector<pipeline::render::PickReadbackResult> Results = Picker.Poll();
	Require(Results.size() == pipeline::render::ViewportPicker::ReadbackSlotCount && Picker.GetPendingCount() == 0,
			"viewport picker did not retire every completed PBO readback");
	Require(Picker.GetAvailableRequestCount() == pipeline::render::ViewportPicker::ReadbackSlotCount,
			"viewport picker did not recover its capacity after retiring readbacks");
	for (const pipeline::render::PickReadbackResult &Result : Results)
		Require(Result.Pick == FirstPick && Result.SourceFrame == PickTable.GetFrameNumber() &&
					PickTable.Resolve(Result.Pick) == FirstObject,
				"viewport picker returned corrupt object identity metadata");
	glDeleteTextures(1, &Texture);
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
	const resource::AssetRecordHandle ReservedRecord = Assets.GetRecord(resource::AssetType::ShaderSource, ReservedPath);
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
	resource::AssetManager Assets(Temporary.GetPath());
	Assets.AddAssetImporter<ReservingValidationImporter>();
	auto Dependent = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, DependentPath);
	auto Failing = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, FailingPath);
	auto Healthy = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, HealthyPath);
	const uint64 DependentGeneration = Dependent.GetPublishedGeneration();
	const uint64 FailingGeneration = Failing.GetPublishedGeneration();
	const uint64 HealthyGeneration = Healthy.GetPublishedGeneration();

	AdvanceValidationWriteTime(DependencyPath);
	Require(Assets.ReloadChangedAssets() == 2 && Dependent.GetPublishedGeneration() == DependentGeneration + 1U,
			"dependency timestamp change did not reload both the dependency asset and its owning asset");

	AdvanceValidationWriteTime(FailingPath);
	AdvanceValidationWriteTime(HealthyPath);
	FailValidationAsset.store(true, std::memory_order_relaxed);
	Require(Assets.ReloadChangedAssets() == 2, "reload pass did not process every changed root asset");
	const resource::AssetRecordHandle FailingRecord = Assets.GetRecord(resource::AssetType::ShaderSource, FailingPath);
	Require(FailingRecord != nullptr && FailingRecord->GetState() == resource::AssetLoadState::Failed && !FailingRecord->GetError().empty(),
			"failed reload did not publish the failed state and diagnostic");
	Require(Failing.GetPublishedGeneration() == FailingGeneration && Failing.Pin() != nullptr,
			"failed reload discarded the prior successfully loaded asset generation");
	Require(Healthy.GetPublishedGeneration() == HealthyGeneration + 1U && Healthy.Pin() != nullptr,
			"one failed reload prevented a later changed asset from reloading");
	FailValidationAsset.store(false, std::memory_order_relaxed);
}

void ValidateAssetDependencyCycleAndRecordLifetime()
{
	TemporaryValidationDirectory Temporary;
	const std::filesystem::path CycleA = Temporary.GetPath() / "CycleA.shader";
	const std::filesystem::path CycleB = Temporary.GetPath() / "CycleB.shader";
	WriteValidationFile(CycleA, "cycle-a");
	WriteValidationFile(CycleB, "cycle-b");

	resource::AssetManager Assets(Temporary.GetPath());
	Assets.AddAssetImporter<ReservingValidationImporter>();
	bool CycleRejected = false;
	try
	{
		(void)Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, CycleA);
	}
	catch (const resource::importer::AssetDependencyCycleException &)
	{
		CycleRejected = true;
	}
	Require(CycleRejected, "recursive asset dependency cycle did not fail with the typed cycle exception");
	const resource::AssetRecordHandle CycleARecord = Assets.GetRecord(resource::AssetType::ShaderSource, CycleA);
	const resource::AssetRecordHandle CycleBRecord = Assets.GetRecord(resource::AssetType::ShaderSource, CycleB);
	Require(CycleARecord && CycleBRecord && CycleARecord->GetState() == resource::AssetLoadState::Failed &&
				CycleBRecord->GetState() == resource::AssetLoadState::Failed,
			"dependency-cycle failure did not transition every participating record to Failed");

	const resource::AssetID GeneratedID = util::UUID::GenerateRandomUUID().ToString();
	resource::AssetHandle<ValidationGPUAsset> Generated = Assets.PublishGeneratedAsset<ValidationGPUAsset>(
		GeneratedID, "__Validation/RecordLifetime.asset", resource::AssetPtr<ValidationGPUAsset>::Make(7U, false, true));
	resource::AssetRecordHandle Record = Assets.GetRecord(GeneratedID);
	Generated.Reset();
	Require(Assets.TryRetireGeneratedAsset(GeneratedID).Status == resource::GeneratedAssetRetirementStatus::Referenced,
			"a generated record retired while a record handle still owned its lifetime");
	Record.Reset();
	Require(Assets.TryRetireGeneratedAsset(GeneratedID).Status == resource::GeneratedAssetRetirementStatus::Retired,
			"generated record did not retire after its final external record handle released");
}

void ValidateFailedImportReservationRollback()
{
	TemporaryValidationDirectory Temporary;
	const std::filesystem::path FailurePath = Temporary.GetPath() / "ReservationFailure.shader";
	const std::filesystem::path ExistingPath = Temporary.GetPath() / "ExistingReservation.shader";
	const std::filesystem::path NewPath = Temporary.GetPath() / "NewReservation.shader";
	WriteValidationFile(FailurePath, "failure");
	WriteValidationFile(ExistingPath, "existing");
	WriteValidationFile(NewPath, "new");

	resource::AssetManager Assets(Temporary.GetPath());
	Assets.AddAssetImporter<ReservingValidationImporter>();
	resource::AssetHandle<ValidationAsset> Existing = Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, ExistingPath);
	bool Failed = false;
	try
	{
		(void)Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, FailurePath);
	}
	catch (const resource::importer::AssetContentValidationException &)
	{
		Failed = true;
	}
	Require(Failed, "reservation rollback fixture did not fail import");
	Require(!Assets.GetRecord(resource::AssetType::ShaderSource, NewPath),
			"failed import retained a newly created untouched reservation record");
	const resource::AssetRecordHandle ExistingRecord = Assets.GetRecord(resource::AssetType::ShaderSource, ExistingPath);
	Require(ExistingRecord && ExistingRecord->GetState() == resource::AssetLoadState::Ready && Existing.Pin() != nullptr,
			"failed import rollback removed or changed a pre-existing reservation record");
}

void ValidateAssetLoadTransactionRollback()
{
	TemporaryValidationDirectory Temporary;
	const std::filesystem::path HealthyPath = Temporary.GetPath() / "TransactionalHealthy.shader";
	const std::filesystem::path FailingPath = Temporary.GetPath() / "Failing.shader";
	WriteValidationFile(HealthyPath, "healthy");
	WriteValidationFile(FailingPath, "failing");

	resource::AssetManager Assets(Temporary.GetPath());
	Assets.AddAssetImporter<ReservingValidationImporter>();
	FailValidationAsset.store(true, std::memory_order_release);
	bool Failed = false;
	try
	{
		resource::AssetLoadTransaction Transaction = Assets.BeginLoadTransaction();
		(void)Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, HealthyPath);
		(void)Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, FailingPath);
		Transaction.Commit();
	}
	catch (const resource::importer::AssetContentValidationException &)
	{
		Failed = true;
	}
	FailValidationAsset.store(false, std::memory_order_release);
	Require(Failed, "asset-load transaction failure fixture did not fail");
	Require(!Assets.GetRecord(resource::AssetType::ShaderSource, HealthyPath) &&
				!Assets.GetRecord(resource::AssetType::ShaderSource, FailingPath),
			"failed asset-load transaction retained records published earlier in the batch");

	const std::filesystem::path ParallelA = Temporary.GetPath() / "ParallelA.shader";
	const std::filesystem::path ParallelB = Temporary.GetPath() / "ParallelB.shader";
	WriteValidationFile(ParallelA, "parallel-a");
	WriteValidationFile(ParallelB, "parallel-b");
	ParallelValidationImports.store(0, std::memory_order_release);
	MaximumParallelValidationImports.store(0, std::memory_order_release);
	ParallelValidationArrivals.store(0, std::memory_order_release);
	auto First = std::async(std::launch::async, [&Assets, &ParallelA]()
							{ return Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, ParallelA).Pin() != nullptr; });
	auto Second = std::async(std::launch::async, [&Assets, &ParallelB]()
							 { return Assets.GetAsset<ValidationAsset>(resource::AssetType::ShaderSource, ParallelB).Pin() != nullptr; });
	Require(First.get() && Second.get() && MaximumParallelValidationImports.load(std::memory_order_acquire) >= 2U,
			"ordinary asset loads were serialized by the exclusive scene-load transaction gate");
}

void ValidateAnimationAssetContracts()
{
	resource::AssetManager Assets;
	const std::vector<resource::SkeletonJoint> Joints{{.ID = 1, .Name = "Root", .ParentIndex = resource::InvalidJointIndex},
													  {.ID = 2, .Name = "Child", .ParentIndex = 0}};
	const auto FirstSkeleton =
		Assets.PublishGeneratedAsset<resource::SkeletonAsset>("ValidationSkeletonA", "__Validation/Animation/SkeletonA.asset",
															  resource::AssetPtr<resource::SkeletonAsset>::Make("SkeletonA", 1, Joints));
	const auto SecondSkeleton =
		Assets.PublishGeneratedAsset<resource::SkeletonAsset>("ValidationSkeletonB", "__Validation/Animation/SkeletonB.asset",
															  resource::AssetPtr<resource::SkeletonAsset>::Make("SkeletonB", 2, Joints));

	auto MakeTrack = []()
	{
		return resource::AnimationJointTrack{
			.ID = 1,
			.Joint = 1,
			.Keys = {{.Time = 0.0f, .Translation = {}, .Rotation = {1.0f, 0.0f, 0.0f, 0.0f}, .Scale = {1.0f, 1.0f, 1.0f}}}};
	};
	const auto FirstClip = Assets.PublishGeneratedAsset<resource::AnimationClipAsset>(
		"ValidationClipA", "__Validation/Animation/ClipA.asset",
		resource::AssetPtr<resource::AnimationClipAsset>::Make(
			"ClipA", FirstSkeleton, 1.0f, 60.0f, std::vector<resource::AnimationJointTrack>{MakeTrack()},
			std::vector<resource::AnimationMorphTrack>{}, std::vector<resource::AnimationEvent>{}));
	const auto SecondClip = Assets.PublishGeneratedAsset<resource::AnimationClipAsset>(
		"ValidationClipB", "__Validation/Animation/ClipB.asset",
		resource::AssetPtr<resource::AnimationClipAsset>::Make(
			"ClipB", SecondSkeleton, 1.0f, 60.0f, std::vector<resource::AnimationJointTrack>{MakeTrack()},
			std::vector<resource::AnimationMorphTrack>{}, std::vector<resource::AnimationEvent>{}));

	bool InvalidQuaternionRejected = false;
	try
	{
		resource::AnimationJointTrack InvalidTrack = MakeTrack();
		InvalidTrack.Keys.front().Rotation = {0.0f, 0.0f, 0.0f, 0.0f};
		(void)resource::AssetPtr<resource::AnimationClipAsset>::Make(
			"Invalid", FirstSkeleton, 1.0f, 60.0f, std::vector<resource::AnimationJointTrack>{std::move(InvalidTrack)},
			std::vector<resource::AnimationMorphTrack>{}, std::vector<resource::AnimationEvent>{});
	}
	catch (const std::invalid_argument &)
	{
		InvalidQuaternionRejected = true;
	}
	Require(InvalidQuaternionRejected, "animation asset accepted a degenerate rotation quaternion");

	bool MixedSkeletonGraphRejected = false;
	try
	{
		const std::vector<resource::AnimationGraphNode> Nodes{
			{.ID = 1, .Type = resource::AnimationGraphNodeType::Clip, .Clip = FirstClip},
			{.ID = 2, .Type = resource::AnimationGraphNodeType::Clip, .Clip = SecondClip},
			{.ID = 3, .Type = resource::AnimationGraphNodeType::Blend, .Inputs = {1, 2}, .ControllingParameter = 4},
			{.ID = 5, .Type = resource::AnimationGraphNodeType::Output, .Inputs = {3}}};
		(void)resource::AssetPtr<resource::AnimationGraphAsset>::Make(
			"Mixed",
			std::vector<resource::AnimationParameterDefinition>{{.ID = 4,
																 .Name = "Blend",
																 .Type = resource::AnimationParameterType::Scalar,
																 .DefaultValue = glm::vec4(0.5f, 0.0f, 0.0f, 0.0f)}},
			Nodes, 5);
	}
	catch (const std::invalid_argument &)
	{
		MixedSkeletonGraphRejected = true;
	}
	Require(MixedSkeletonGraphRejected, "animation graph accepted clips using different skeleton assets");

	bool InvalidTopologyRejected = false;
	try
	{
		(void)resource::AssetPtr<resource::RetargetProfileAsset>::Make(
			FirstSkeleton, SecondSkeleton, std::vector<resource::RetargetJointMapping>{{.Source = 2, .Destination = 1}});
	}
	catch (const std::invalid_argument &)
	{
		InvalidTopologyRejected = true;
	}
	Require(InvalidTopologyRejected, "retarget profile accepted an incompatible root/non-root mapping");
}

void ValidateGPUAssetGenerationRollback(pipeline::device::Device &Device)
{
	resource::AssetManager Assets;
	const resource::AssetID AssetID = util::UUID::GenerateRandomUUID().ToString();
	const resource::AssetID FirstDependency = util::UUID::GenerateRandomUUID().ToString();
	const resource::AssetID ReplacementDependency = util::UUID::GenerateRandomUUID().ToString();
	resource::AssetHandle<ValidationGPUAsset> Handle = Assets.PublishGeneratedAsset<ValidationGPUAsset>(
		AssetID, "__Validation/GPUGeneration.asset", resource::AssetPtr<ValidationGPUAsset>::Make(1U, false, true), {FirstDependency});
	const uint64 StableGeneration = Handle.GetPublishedGeneration();
	Require(Handle.Pin()->GetMarker() == 1U && Handle.GetState() == resource::AssetLoadState::Ready,
			"generated GPU rollback fixture did not publish its initial ready generation");

	(void)Assets.PublishGeneratedAsset<ValidationGPUAsset>(AssetID, "__Validation/GPUGeneration.asset",
														   resource::AssetPtr<ValidationGPUAsset>::Make(2U, true, false),
														   {ReplacementDependency});
	Require(Handle.GetPublishedGeneration() == StableGeneration && Handle.Pin()->GetMarker() == 1U,
			"staged GPU candidate replaced the active generation before realization");
	Require(!Assets.RealizeGPU(Device, AssetID), "injected failed GPU candidate unexpectedly realized");
	const std::optional<resource::AssetRecordSnapshot> FailedSnapshot = Assets.SnapshotRecord(AssetID);
	Require(FailedSnapshot.has_value() && FailedSnapshot->State == resource::AssetLoadState::Ready && !FailedSnapshot->Error.empty() &&
				FailedSnapshot->Dependencies == std::vector<resource::AssetID>{FirstDependency} && Handle.Pin()->GetMarker() == 1U &&
				Handle.GetPublishedGeneration() == StableGeneration,
			"failed GPU candidate did not preserve the complete previous ready generation");

	(void)Assets.PublishGeneratedAsset<ValidationGPUAsset>(
		AssetID, "__Validation/GPUGeneration.asset", resource::AssetPtr<ValidationGPUAsset>::Make(3U, true, true), {ReplacementDependency});
	Require(Assets.RealizeGPU(Device, AssetID), "valid staged GPU candidate did not realize");
	const std::optional<resource::AssetRecordSnapshot> PublishedSnapshot = Assets.SnapshotRecord(AssetID);
	Require(PublishedSnapshot.has_value() && PublishedSnapshot->State == resource::AssetLoadState::Ready &&
				PublishedSnapshot->Error.empty() &&
				PublishedSnapshot->Dependencies == std::vector<resource::AssetID>{ReplacementDependency} &&
				Handle.Pin()->GetMarker() == 3U && Handle.GetPublishedGeneration() == StableGeneration + 1U,
			"successful GPU candidate did not atomically publish its asset and dependency generation");
}

void ValidateImmutableSceneSnapshot()
{
	editor::document::SceneDocument Document("Immutable render snapshot");
	const world::ObjectHandle Parent = Document.CreateObject("Parent");
	const world::ObjectHandle Object = Document.CreateObject("Point light", Parent);
	const auto Light = Document.GetScene().AddComponent<components::CObjectPointLightComponent>(Object);
	const auto Transform = Document.GetScene().GetComponent<components::CObjectTransformComponent>(Object);
	const auto ParentTransform = Document.GetScene().GetComponent<components::CObjectTransformComponent>(Parent);
	const auto ParentIdentity = Document.GetScene().GetComponent<components::CObjectIdentityComponent>(Parent);
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(ParentTransform).SetPosition({10.0f, 0.0f, 0.0f});
		Access.Resolve(Transform).SetPosition({1.0f, 2.0f, 3.0f});
		Access.Resolve(Light).SetLuminousPowerLumens(4'000.0f);
	}
	const pipeline::render::SceneRenderSnapshot Snapshot = pipeline::render::SceneRenderSnapshotBuilder::Build(Document.GetScene());
	{
		auto Access = Document.GetScene().Read();
		Require(glm::length(glm::vec3(Access.GetWorldTransform(Object)[3]) - glm::vec3(11.0f, 2.0f, 3.0f)) < 1.0e-5f,
				"scene read access did not resolve a hierarchy-aware world transform");
	}
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(Transform).SetPosition({9.0f, 8.0f, 7.0f});
		Access.Resolve(Light).SetLuminousPowerLumens(8'000.0f);
	}
	Require(Snapshot.ObjectCount == 2 && Snapshot.PointLights.size() == 1 &&
				glm::length(Snapshot.PointLights.front().Position - glm::vec3(11.0f, 2.0f, 3.0f)) < 1.0e-5f,
			"render snapshot did not publish an immutable hierarchy-resolved world transform");
	Require(Snapshot.DebugLines.empty() && Snapshot.DebugBounds.empty(),
			"default render snapshot unexpectedly paid for editor-only debug geometry");
	const pipeline::render::SceneRenderSnapshot LightDebugSnapshot =
		pipeline::render::SceneRenderSnapshotBuilder::Build(Document.GetScene(), {.IncludeLights = true});
	Require(!LightDebugSnapshot.DebugLines.empty() &&
				std::ranges::all_of(LightDebugSnapshot.DebugLines, [](const pipeline::render::SceneDebugLine &Line)
									{ return Line.Category == pipeline::render::SceneDebugLineCategory::Light; }),
			"light-overlay snapshot did not generate categorized debug geometry");
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(ParentIdentity).SetEnabled(false);
	}
	Require(pipeline::render::SceneRenderSnapshotBuilder::Build(Document.GetScene()).PointLights.empty(),
			"disabled parent identity did not suppress descendant rendering");
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(ParentIdentity).SetEnabled(true);
		Access.Resolve(ParentIdentity).SetEditorVisible(false);
	}
	Require(
		pipeline::render::SceneRenderSnapshotBuilder::Build(Document.GetScene()).PointLights.size() == 1 &&
			pipeline::render::SceneRenderSnapshotBuilder::Build(Document.GetScene(), {.RespectEditorVisibility = true}).PointLights.empty(),
		"editor visibility did not propagate independently from runtime enabled state");
}

void ValidatePrivateMaterialExtraction(pipeline::device::Device &Device)
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLMaterialExtractionValidation-" + util::UUID::GenerateRandomUUID().ToString());
	std::filesystem::create_directories(Root);
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};

	resource::AssetManager Assets(Root);
	editor::asset::PrimitiveMeshFactory Factory(Assets);
	editor::document::SceneDocument Document("Private material extraction");
	const world::ObjectHandle Object = Document.CreateObject("ValidationCube");
	const resource::AssetHandle<resource::ModelAsset> Model = Factory.GetModel(editor::asset::PrimitiveShape::Box);
	(void)Document.GetScene().AddComponent<components::CObjectMeshComponent>(Object, Model);
	const resource::AssetPtr<resource::ModelAsset> PinnedModel = Model.Pin();
	const resource::ModelMeshInstance &Instance = PinnedModel->GetMeshInstances().front();
	const resource::AssetPtr<resource::MeshAsset> Mesh = Instance.Mesh.Pin();
	const resource::MeshMaterialSlot &Slot = Mesh->GetMaterialSlots().front();
	const resource::AssetPtr<resource::MaterialInterfaceAsset> Parent = Slot.DefaultMaterial.Pin();
	resource::PBRMaterialFactors Factors = Parent->GetFactors();
	const glm::vec4 SelectedColor(0.0f, 0.0341006815f, 0.1023017764f, 1.0f);
	Factors.BaseColor = SelectedColor;
	const resource::AssetID MaterialID = util::UUID::GenerateRandomUUID().ToString();
	const resource::AssetHandle<resource::MaterialInstanceAsset> Material = Assets.PublishGeneratedAsset<resource::MaterialInstanceAsset>(
		MaterialID, Root / "ValidationCube_1_1.materialinstance",
		resource::AssetPtr<resource::MaterialInstanceAsset>::Make(
			"ValidationCube_1_1", Slot.DefaultMaterial, Parent->GetPipelineContract(), Factors,
			std::vector<resource::MaterialTextureBinding>(Parent->GetTextures().begin(), Parent->GetTextures().end())),
		{Slot.DefaultMaterial.GetID()});
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object)).SetMaterialOverride(Instance.ID, Slot.ID, Material);
	}

	const pipeline::render::SceneRenderSnapshot Snapshot = pipeline::render::SceneRenderSnapshotBuilder::Build(Document.GetScene());
	pipeline::mesh::MeshGPUCache MeshCache(Device);
	(void)MeshCache.Realize(Mesh, 1).GetLOD(0, 1);
	pipeline::render::RenderTransformHistory PreviousTransforms;
	pipeline::render::RenderTransformHistory CurrentTransforms;
	pipeline::render::SceneExtractorScratch Scratch;
	pipeline::render::SceneExtractor Extractor(Device, MeshCache, Assets, PreviousTransforms, 0, CurrentTransforms, 1, Scratch);
	Camera Camera(0.1f, 45.0f, 0.1f, 1'000.0f);
	SceneCollection Collection;
	Collection.BeginFrame(1);
	Extractor.Extract(Snapshot, Camera, Collection);
	Collection.Seal();
	Require(!Collection.GetRenderItems().empty() &&
				std::ranges::all_of(Collection.GetRenderItems(), [&SelectedColor](const pipeline::render::RenderItem &Item)
									{ return glm::all(glm::equal(Item.Material.BaseColorFactor, SelectedColor)); }),
			"scene extraction replaced a private material override with its default material base color");
	MeshCache.Clear();
}
} // namespace

void RunDeterministicRenderCoreChecks(pipeline::device::Device &Device)
{
	std::cerr << "[Validation] Memory pools\n";
	ValidateMemoryPools();
	std::cerr << "[Validation] Transform safety\n";
	ValidateTransformSafety();
	std::cerr << "[Validation] Scene storage and commands\n";
	ValidateSceneStorageAndCommands();
	std::cerr << "[Validation] Transactional light uploads\n";
	ValidateTransactionalLightUploads();
	std::cerr << "[Validation] Event listener mutation\n";
	ValidateEventListenerMutation();
	std::cerr << "[Validation] Task scheduler\n";
	ValidateTaskScheduler();
	std::cerr << "[Validation] Editor commands and reflection\n";
	ValidateEditorCommandsAndReflection();
	std::cerr << "[Validation] Project and scene document\n";
	ValidateProjectAndSceneDocument();
	std::cerr << "[Validation] Reserved asset loading\n";
	ValidateReservedAssetLoading();
	std::cerr << "[Validation] Asset reload pipeline\n";
	ValidateAssetReloadPipeline();
	ValidateAssetDependencyCycleAndRecordLifetime();
	ValidateFailedImportReservationRollback();
	ValidateAssetLoadTransactionRollback();
	ValidateAnimationAssetContracts();
	std::cerr << "[Validation] GPU asset generation rollback\n";
	ValidateGPUAssetGenerationRollback(Device);
	std::cerr << "[Validation] Immutable scene snapshot\n";
	ValidateImmutableSceneSnapshot();
	std::cerr << "[Validation] Private material extraction\n";
	ValidatePrivateMaterialExtraction(Device);
	std::cerr << "[Validation] Render-graph dependencies\n";
	ValidateGraphDependencies(Device);
	std::cerr << "[Validation] Render-graph resource lifetimes\n";
	ValidateGraphResourceLifetimes(Device);
	std::cerr << "[Validation] Maximum shadow-memory budget\n";
	ValidateMaximumShadowBudgetAllocation(Device);
	std::cerr << "[Validation] Scene preparation\n";
	ValidateScenePreparation(Device);
	std::cerr << "[Validation] GPU visibility compaction\n";
	ValidateGPUVisibilityCompaction(Device);
	std::cerr << "[Validation] Shader resource contracts\n";
	ValidateShaderResourceContracts(Device);
	std::cerr << "[Validation] Viewport picker\n";
	ValidateViewportPicker(Device);
	std::cerr << "[Validation] Render-thread context transfer\n";
	ValidateRenderThreadContextTransfer(Device);
}
} // namespace pipeline::validation
