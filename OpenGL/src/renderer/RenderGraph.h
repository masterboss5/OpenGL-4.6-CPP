#pragma once

#include "src/types.h"

#include <GL/glew.h>
#include <functional>
#include <glm.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pipeline::shader
{
class GraphicsPipeline;
}

namespace pipeline::device
{
class Device;
}

namespace renderer::graph
{
struct Extent2D final
{
	uint32 Width = 0;
	uint32 Height = 0;
	[[nodiscard]] bool IsValid() const noexcept
	{
		return Width != 0 && Height != 0;
	}
};
enum class TextureFormat : uint8
{
	Depth32Float,
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

struct BufferDescription final
{
	std::string DebugName;
	uint64 SizeInBytes = 0;
	GLbitfield StorageFlags = GL_DYNAMIC_STORAGE_BIT;
	bool Persistent = false;
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
class RenderGraphContext final
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
};

struct RenderPassDescription final
{
	std::string Name;
	PassQueue Queue = PassQueue::Graphics;
	std::vector<TextureHandle> ReadTextures;
	std::vector<BufferHandle> ReadBuffers;
	std::vector<TextureAttachment> ColorAttachments;
	std::optional<DepthAttachment> DepthAttachment;
	std::vector<TextureHandle> WriteTextures;
	std::vector<BufferHandle> WriteBuffers;
	std::function<void(RenderGraphContext &)> Execute;
};

class RenderGraph final
{
  public:
	explicit RenderGraph(pipeline::device::Device &Device);
	~RenderGraph();
	RenderGraph(const RenderGraph &) = delete;
	RenderGraph &operator=(const RenderGraph &) = delete;

	void BeginFrame(Extent2D Extent);
	[[nodiscard]] TextureHandle CreateTexture(TextureDescription Description);
	[[nodiscard]] TextureHandle ImportTexture(TextureDescription Description, GLuint Texture);
	[[nodiscard]] BufferHandle CreateBuffer(BufferDescription Description);
	[[nodiscard]] BufferHandle ImportBuffer(BufferDescription Description, GLuint Buffer);
	[[nodiscard]] PassHandle AddPass(RenderPassDescription Description);
	void Compile();
	void Execute();
	void Reset();
	[[nodiscard]] bool IsCompiled() const noexcept;

  private:
	pipeline::device::Device *Device = nullptr;
	struct TextureResource;
	struct BufferResource;
	struct PassResource;
	struct PhysicalTexture;
	struct PhysicalBuffer;
	Extent2D FrameExtent;
	bool Compiled = false;
	uint64 FrameSerial = 0;
	std::vector<TextureResource> Textures;
	std::vector<BufferResource> Buffers;
	std::vector<PassResource> Passes;
	std::vector<PhysicalTexture> TexturePool;
	std::vector<PhysicalBuffer> BufferPool;

	[[nodiscard]] const TextureResource &GetTextureResource(TextureHandle Handle) const;
	[[nodiscard]] const BufferResource &GetBufferResource(BufferHandle Handle) const;
	[[nodiscard]] const PassResource &GetPassResource(PassHandle Handle) const;
	void Validate() const;
	void AllocateResources();
	void CreatePassFramebuffers();
	void ReleasePassFramebuffers() noexcept;
	friend class RenderGraphContext;
};
} // namespace renderer::graph
