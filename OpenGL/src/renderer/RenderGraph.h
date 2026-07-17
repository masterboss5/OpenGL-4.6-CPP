#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <glm.hpp>

#include "src/types.h"

namespace pipeline::shader
{
	class GraphicsPipeline;
}

namespace renderer::graph
{
	struct Extent2D final { uint32 width = 0; uint32 height = 0; [[nodiscard]] bool isValid() const noexcept { return width != 0 && height != 0; } };
	enum class TextureFormat : uint8 { Depth32Float, RGBA16Float, RG16Float, R32UnsignedInteger, R32Float, R8Unorm };
	enum class TextureDimension : uint8 { Texture2D, Texture2DArray, TextureCubeArray, Texture2DMultisample, Texture2DMultisampleArray };
	enum class PassQueue : uint8 { Graphics, Compute };
	enum class LoadOperation : uint8 { Load, Clear, Discard };
	enum class StoreOperation : uint8 { Store, Discard };

	struct TextureHandle final { uint32 value = ~uint32 { 0 }; [[nodiscard]] bool isValid() const noexcept { return value != ~uint32 { 0 }; } };
	struct BufferHandle final { uint32 value = ~uint32 { 0 }; [[nodiscard]] bool isValid() const noexcept { return value != ~uint32 { 0 }; } };
	struct PassHandle final { uint32 value = ~uint32 { 0 }; [[nodiscard]] bool isValid() const noexcept { return value != ~uint32 { 0 }; } };

	struct TextureDescription final
	{
		std::string debugName;
		Extent2D extent;
		TextureFormat format = TextureFormat::RGBA16Float;
		TextureDimension dimension = TextureDimension::Texture2D;
		uint32 mipCount = 1;
		uint32 layers = 1;
		uint32 sampleCount = 1;
		bool persistent = false;
	};

	struct BufferDescription final { std::string debugName; uint64 sizeInBytes = 0; GLbitfield storageFlags = GL_DYNAMIC_STORAGE_BIT; bool persistent = false; };
	struct TextureAttachment final { TextureHandle texture; LoadOperation load = LoadOperation::Load; StoreOperation store = StoreOperation::Store; glm::vec4 clearColor { 0.0f }; };
	struct DepthAttachment final { TextureHandle texture; LoadOperation load = LoadOperation::Load; StoreOperation store = StoreOperation::Store; float32 clearDepth = 0.0f; };

	class RenderGraph;
	class RenderGraphContext final
	{
	public:
		[[nodiscard]] GLuint getTexture(TextureHandle handle) const;
		[[nodiscard]] GLuint getBuffer(BufferHandle handle) const;
		[[nodiscard]] Extent2D getExtent(TextureHandle handle) const;
		void bindPassFramebuffer() const;
		void validateGraphicsPipelineTargets(const pipeline::shader::GraphicsPipeline& pipeline) const;
	private:
		friend class RenderGraph;
		RenderGraphContext(const RenderGraph& graph, PassHandle pass) : graph(graph), pass(pass) {}
		const RenderGraph& graph;
		PassHandle pass;
	};

	struct RenderPassDescription final
	{
		std::string name;
		PassQueue queue = PassQueue::Graphics;
		std::vector<TextureHandle> readTextures;
		std::vector<BufferHandle> readBuffers;
		std::vector<TextureAttachment> colorAttachments;
		std::optional<DepthAttachment> depthAttachment;
		std::vector<TextureHandle> writeTextures;
		std::vector<BufferHandle> writeBuffers;
		std::function<void(RenderGraphContext&)> execute;
	};

	class RenderGraph final
	{
	public:
		RenderGraph();
		~RenderGraph();
		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;

		void beginFrame(Extent2D extent);
		[[nodiscard]] TextureHandle createTexture(TextureDescription description);
		[[nodiscard]] TextureHandle importTexture(TextureDescription description, GLuint texture);
		[[nodiscard]] BufferHandle createBuffer(BufferDescription description);
		[[nodiscard]] BufferHandle importBuffer(BufferDescription description, GLuint buffer);
		[[nodiscard]] PassHandle addPass(RenderPassDescription description);
		void compile();
		void execute();
		void reset();
		[[nodiscard]] bool isCompiled() const noexcept;

	private:
		struct TextureResource;
		struct BufferResource;
		struct PassResource;
		struct PhysicalTexture;
		struct PhysicalBuffer;
		Extent2D frameExtent;
		bool compiled = false;
		std::vector<TextureResource> textures;
		std::vector<BufferResource> buffers;
		std::vector<PassResource> passes;
		std::vector<PhysicalTexture> texturePool;
		std::vector<PhysicalBuffer> bufferPool;

		[[nodiscard]] const TextureResource& getTextureResource(TextureHandle handle) const;
		[[nodiscard]] const BufferResource& getBufferResource(BufferHandle handle) const;
		[[nodiscard]] const PassResource& getPassResource(PassHandle handle) const;
		void validate() const;
		void allocateResources();
		void createPassFramebuffers();
		void releasePassFramebuffers() noexcept;
		friend class RenderGraphContext;
	};
}
