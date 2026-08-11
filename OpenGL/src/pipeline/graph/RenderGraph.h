#pragma once

#include "src/types.h"
#include "src/pipeline/device/Device.h"

#include <GL/glew.h>
#include <functional>
#include <initializer_list>
#include <glm.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pipeline::shader
{
class GraphicsPipeline;
}

namespace pipeline::graph
{
class RenderPass;

struct Extent2D final
{
	uint32 Width = 0;
	uint32 Height = 0;
	[[nodiscard]] bool operator==(const Extent2D &) const noexcept = default;
	[[nodiscard]] bool IsValid() const noexcept
	{
		return Width != 0 && Height != 0;
	}
};
enum class TextureFormat : uint8
{
	Depth32Float,
	RGBA8SRGB,
	RGBA16Float,
	RG16Float,
	R32UnsignedInteger,
	R32Float,
	R8Unorm
};
enum class TextureDimension : uint8
{
	Texture2D,
	Texture2DArray,
	TextureCubeArray,
	Texture2DMultisample,
	Texture2DMultisampleArray
};
enum class PassQueue : uint8
{
	Graphics,
	Compute
};
enum class LoadOperation : uint8
{
	Load,
	Clear,
	Discard
};
enum class StoreOperation : uint8
{
	Store,
	Discard
};

struct TextureHandle final
{
	uint32 Value = ~uint32{0};
	[[nodiscard]] bool IsValid() const noexcept
	{
		return Value != ~uint32{0};
	}
};
struct BufferHandle final
{
	uint32 Value = ~uint32{0};
	[[nodiscard]] bool IsValid() const noexcept
	{
		return Value != ~uint32{0};
	}
};
struct PassHandle final
{
	uint32 Value = ~uint32{0};
	[[nodiscard]] bool IsValid() const noexcept
	{
		return Value != ~uint32{0};
	}
};

struct TextureDescription final
{
	std::string DebugName;
	Extent2D Extent;
	TextureFormat Format = TextureFormat::RGBA16Float;
	TextureDimension Dimension = TextureDimension::Texture2D;
	uint32 MipCount = 1;
	uint32 Layers = 1;
	uint32 SampleCount = 1;
	bool Persistent = false;
};

struct ExportedTexture final
{
	GLuint Texture = 0;
	Extent2D Extent;
	TextureFormat Format = TextureFormat::RGBA16Float;
	TextureDimension Dimension = TextureDimension::Texture2D;
	uint32 MipCount = 1;
	uint32 Layers = 1;
	uint32 SampleCount = 1;
	uint64 FrameSerial = 0;
	// Zero denotes a graph that is not owned by a renderer view.
	uint64 ViewIdentity = 0;
	uint64 ViewGeneration = 0;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Texture != 0 && this->Extent.IsValid() && this->FrameSerial != 0 &&
			   (this->ViewIdentity == 0 || this->ViewGeneration != 0);
	}
};

struct BufferDescription final
{
	std::string DebugName;
	uint64 SizeInBytes = 0;
	GLbitfield StorageFlags = GL_DYNAMIC_STORAGE_BIT;
	bool Persistent = false;
};

enum class BufferUsage : uint16
{
	None = 0,
	ShaderStorage = 1U << 0U,
	Uniform = 1U << 1U,
	Vertex = 1U << 2U,
	Index = 1U << 3U,
	Indirect = 1U << 4U,
	AtomicCounter = 1U << 5U,
	TextureBuffer = 1U << 6U,
	PixelBuffer = 1U << 7U,
	Transfer = 1U << 8U
};

[[nodiscard]] constexpr BufferUsage operator|(const BufferUsage Left, const BufferUsage Right) noexcept
{
	return static_cast<BufferUsage>(static_cast<uint16>(Left) | static_cast<uint16>(Right));
}

struct BufferAccess final
{
	BufferHandle Buffer;
	BufferUsage Usage = BufferUsage::ShaderStorage;

	BufferAccess() = default;
	BufferAccess(const BufferHandle Buffer, const BufferUsage Usage = BufferUsage::ShaderStorage) : Buffer(Buffer), Usage(Usage)
	{
	}
};
struct TextureAttachment final
{
	TextureHandle Texture;
	LoadOperation Load = LoadOperation::Load;
	StoreOperation Store = StoreOperation::Store;
	glm::vec4 ClearColor{0.0f};
};
struct DepthAttachment final
{
	TextureHandle Texture;
	LoadOperation Load = LoadOperation::Load;
	StoreOperation Store = StoreOperation::Store;
	float32 ClearDepth = 0.0f;
};

class RenderGraph;
class ENGINE_API RenderGraphContext final
{
  public:
	[[nodiscard]] GLuint GetTexture(TextureHandle Handle) const;
	[[nodiscard]] GLuint GetBuffer(BufferHandle Handle) const;
	[[nodiscard]] Extent2D GetExtent(TextureHandle Handle) const;
	void BindPassFramebuffer() const;
	void ValidateGraphicsPipelineTargets(const pipeline::shader::GraphicsPipeline &Pipeline) const;

  private:
	friend class RenderGraph;
	RenderGraphContext(const RenderGraph &Graph, PassHandle Pass) : Graph(Graph), Pass(Pass)
	{
	}
	const RenderGraph &Graph;
	PassHandle Pass;
	void RequireTextureAccess(TextureHandle Handle) const;
	void RequireBufferAccess(BufferHandle Handle) const;
};

struct RenderPassDescription final
{
	std::string Name;
	PassQueue Queue = PassQueue::Graphics;
	std::vector<TextureHandle> ReadTextures;
	std::vector<BufferAccess> ReadBuffers;
	std::vector<TextureAttachment> ColorAttachments;
	std::optional<DepthAttachment> DepthAttachment;
	std::vector<TextureHandle> WriteTextures;
	std::vector<BufferAccess> WriteBuffers;
	std::function<void(RenderGraphContext &)> Execute;
};

struct RenderGraphInspection final
{
	uint64 FrameSerial = 0;
	std::vector<string> Passes;
	std::vector<string> Textures;
	std::vector<string> Buffers;
};

class ENGINE_API RenderGraph final
{
  public:
	explicit RenderGraph(pipeline::device::Device &Device);
	~RenderGraph();
	RenderGraph(const RenderGraph &) = delete;
	RenderGraph &operator=(const RenderGraph &) = delete;

	void BeginFrame(Extent2D Extent, uint64 ViewIdentity = 0, uint64 ViewGeneration = 0);
	[[nodiscard]] TextureHandle CreateTexture(TextureDescription Description);
	[[nodiscard]] TextureHandle CreateTexture(string_view DebugName, Extent2D Extent, TextureFormat Format = TextureFormat::RGBA16Float,
											  TextureDimension Dimension = TextureDimension::Texture2D, uint32 MipCount = 1,
											  uint32 Layers = 1, uint32 SampleCount = 1, bool Persistent = false);
	[[nodiscard]] TextureHandle ImportTexture(TextureDescription Description, GLuint Texture);
	[[nodiscard]] TextureHandle ImportTexture(string_view DebugName, Extent2D Extent, TextureFormat Format, GLuint Texture,
											  TextureDimension Dimension = TextureDimension::Texture2D, uint32 MipCount = 1,
											  uint32 Layers = 1, uint32 SampleCount = 1, bool Persistent = false);
	[[nodiscard]] BufferHandle CreateBuffer(BufferDescription Description);
	[[nodiscard]] BufferHandle CreateBuffer(string_view DebugName, uint64 SizeInBytes, GLbitfield StorageFlags = GL_DYNAMIC_STORAGE_BIT,
											bool Persistent = false);
	[[nodiscard]] BufferHandle ImportBuffer(BufferDescription Description, GLuint Buffer);
	[[nodiscard]] BufferHandle ImportBuffer(string_view DebugName, uint64 SizeInBytes, GLbitfield StorageFlags, GLuint Buffer,
											bool Persistent = false);
	[[nodiscard]] PassHandle AddPass(RenderPass Pass);
	// Compatibility entry point for callers that build a description inline;
	// RenderGraph still takes ownership through RenderPass.
	[[nodiscard]] PassHandle AddPass(RenderPassDescription Description);
	[[nodiscard]] PassHandle AddPass(string_view Name, PassQueue Queue, std::initializer_list<TextureHandle> ReadTextures,
									 std::initializer_list<BufferAccess> ReadBuffers,
									 std::initializer_list<TextureAttachment> ColorAttachments, std::optional<DepthAttachment> Depth,
									 std::initializer_list<TextureHandle> WriteTextures, std::initializer_list<BufferAccess> WriteBuffers,
									 std::function<void(RenderGraphContext &)> Execute);
	void ExportTexture(TextureHandle Handle);
	[[nodiscard]] ExportedTexture GetExportedTexture(TextureHandle Handle) const;
	void Compile();
	void Execute();
	void Reset();
	[[nodiscard]] bool IsCompiled() const noexcept;
	void InspectInto(RenderGraphInspection &Result) const;
	[[nodiscard]] RenderGraphInspection Inspect() const;

  private:
	pipeline::device::DeviceHandle Device;
	struct TextureResource;
	struct BufferResource;
	struct PassResource;
	struct PhysicalTexture;
	struct PhysicalBuffer;
	Extent2D FrameExtent;
	bool Compiled = false;
	uint64 FrameSerial = 0;
	uint64 ViewIdentity = 0;
	uint64 ViewGeneration = 0;
	std::vector<TextureResource> Textures;
	usize TextureCount = 0;
	std::vector<BufferResource> Buffers;
	usize BufferCount = 0;
	std::vector<PassResource> Passes;
	usize PassCount = 0;
	std::vector<PhysicalTexture> TexturePool;
	std::vector<PhysicalBuffer> BufferPool;
	uint64 NextTextureAllocationGeneration = 1;
	std::vector<GLuint> FramebufferPool;
	usize FramebufferCursor = 0;
	std::vector<bool> TextureWrittenScratch;
	std::vector<bool> BufferWrittenScratch;
	std::vector<GLenum> DrawBuffersScratch;

	[[nodiscard]] const TextureResource &GetTextureResource(TextureHandle Handle) const;
	[[nodiscard]] const BufferResource &GetBufferResource(BufferHandle Handle) const;
	[[nodiscard]] const PassResource &GetPassResource(PassHandle Handle) const;
	[[nodiscard]] TextureHandle CreateTextureInternal(string_view DebugName, Extent2D Extent, TextureFormat Format,
													  TextureDimension Dimension, uint32 MipCount, uint32 Layers, uint32 SampleCount,
													  bool Persistent);
	[[nodiscard]] BufferHandle CreateBufferInternal(string_view DebugName, uint64 SizeInBytes, GLbitfield StorageFlags, bool Persistent);
	void Validate();
	void AllocateResources();
	void CreatePassFramebuffers();
	void ReleasePassFramebuffers() noexcept;
	friend class RenderGraphContext;
};
} // namespace pipeline::graph
