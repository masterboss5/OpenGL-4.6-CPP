#include "RenderGraph.h"

#include <algorithm>
#include <stdexcept>

#include "src/pipeline/device/OpenGLRuntime.h"
#include "src/pipeline/shader/GraphicsPipeline.h"

namespace renderer::graph
{
	struct RenderGraph::TextureResource final { TextureDescription description; GLuint importedID = 0; uint32 physicalIndex = ~uint32 { 0 }; uint32 firstUse = ~uint32 { 0 }; uint32 lastUse = 0; };
	struct RenderGraph::BufferResource final { BufferDescription description; GLuint importedID = 0; uint32 physicalIndex = ~uint32 { 0 }; uint32 firstUse = ~uint32 { 0 }; uint32 lastUse = 0; };
	struct RenderGraph::PassResource final { RenderPassDescription description; GLuint framebuffer = 0; };
	struct RenderGraph::PhysicalTexture final { TextureDescription description; GLuint texture = 0; uint32 lastUse = 0; };
	struct RenderGraph::PhysicalBuffer final { BufferDescription description; GLuint buffer = 0; uint32 lastUse = 0; };

	RenderGraph::RenderGraph() = default;

	namespace
	{
		[[nodiscard]] GLenum toGLInternalFormat(TextureFormat format)
		{
			switch (format) { case TextureFormat::Depth32Float: return GL_DEPTH_COMPONENT32F; case TextureFormat::RGBA16Float: return GL_RGBA16F; case TextureFormat::RG16Float: return GL_RG16F; case TextureFormat::R32UnsignedInteger: return GL_R32UI; case TextureFormat::R32Float: return GL_R32F; case TextureFormat::R8Unorm: return GL_R8; }
			throw std::logic_error("Unknown render graph texture format");
		}
		[[nodiscard]] GLenum toGLTarget(TextureDimension dimension)
		{
			switch (dimension) { case TextureDimension::Texture2D: return GL_TEXTURE_2D; case TextureDimension::Texture2DArray: return GL_TEXTURE_2D_ARRAY; case TextureDimension::TextureCubeArray: return GL_TEXTURE_CUBE_MAP_ARRAY; case TextureDimension::Texture2DMultisample: return GL_TEXTURE_2D_MULTISAMPLE; case TextureDimension::Texture2DMultisampleArray: return GL_TEXTURE_2D_MULTISAMPLE_ARRAY; }
			throw std::logic_error("Unknown render graph texture dimension");
		}
		[[nodiscard]] bool isMultisampled(TextureDimension dimension)
		{
			return dimension == TextureDimension::Texture2DMultisample || dimension == TextureDimension::Texture2DMultisampleArray;
		}
		[[nodiscard]] bool sameTexture(const TextureDescription& left, const TextureDescription& right)
		{
			return left.extent.width == right.extent.width && left.extent.height == right.extent.height && left.format == right.format && left.dimension == right.dimension && left.mipCount == right.mipCount && left.layers == right.layers && left.sampleCount == right.sampleCount;
		}

		template<typename PassResourceType>
		void invalidateDiscardedAttachments(const PassResourceType& pass, bool beforeExecution)
		{
			if (pass.framebuffer == 0) return;
			std::vector<GLenum> attachments;
			for (uint32 colorIndex = 0; colorIndex < pass.description.colorAttachments.size(); ++colorIndex)
			{
				const TextureAttachment& attachment = pass.description.colorAttachments[colorIndex];
				const bool discard = beforeExecution ? attachment.load == LoadOperation::Discard : attachment.store == StoreOperation::Discard;
				if (discard) attachments.push_back(GL_COLOR_ATTACHMENT0 + colorIndex);
			}
			if (pass.description.depthAttachment)
			{
				const DepthAttachment& attachment = *pass.description.depthAttachment;
				const bool discard = beforeExecution ? attachment.load == LoadOperation::Discard : attachment.store == StoreOperation::Discard;
				if (discard) attachments.push_back(GL_DEPTH_ATTACHMENT);
			}
			if (!attachments.empty()) glInvalidateNamedFramebufferData(pass.framebuffer, static_cast<GLsizei>(attachments.size()), attachments.data());
		}
	}

	RenderGraph::~RenderGraph()
	{
		this->releasePassFramebuffers();
		for (PhysicalTexture& texture : texturePool) if (texture.texture != 0) glDeleteTextures(1, &texture.texture);
		for (PhysicalBuffer& buffer : bufferPool) if (buffer.buffer != 0) glDeleteBuffers(1, &buffer.buffer);
	}
	void RenderGraph::beginFrame(Extent2D extent) { if (!extent.isValid()) throw std::invalid_argument("Render graph requires a non-zero frame extent"); this->reset(); this->frameExtent = extent; for (PhysicalTexture& texture : texturePool) texture.lastUse = ~uint32 { 0 }; for (PhysicalBuffer& buffer : bufferPool) buffer.lastUse = ~uint32 { 0 }; }
	TextureHandle RenderGraph::createTexture(TextureDescription description) { if (compiled) throw std::logic_error("Cannot create render graph resources after compile"); if (!description.extent.isValid()) description.extent = frameExtent; const bool multisampled = isMultisampled(description.dimension); if (!description.extent.isValid() || description.mipCount == 0 || description.layers == 0 || description.sampleCount == 0 || (multisampled && (description.sampleCount == 1 || description.mipCount != 1)) || (!multisampled && description.sampleCount != 1)) throw std::invalid_argument("Invalid render graph texture description"); textures.push_back({ .description = std::move(description) }); return { static_cast<uint32>(textures.size() - 1) }; }
	TextureHandle RenderGraph::importTexture(TextureDescription description, GLuint texture) { if (texture == 0) throw std::invalid_argument("Cannot import OpenGL texture 0"); TextureHandle handle = createTexture(std::move(description)); textures[handle.value].importedID = texture; return handle; }
	BufferHandle RenderGraph::createBuffer(BufferDescription description) { if (compiled || description.sizeInBytes == 0) throw std::invalid_argument("Invalid render graph buffer description"); buffers.push_back({ .description = std::move(description) }); return { static_cast<uint32>(buffers.size() - 1) }; }
	BufferHandle RenderGraph::importBuffer(BufferDescription description, GLuint buffer) { if (buffer == 0) throw std::invalid_argument("Cannot import OpenGL buffer 0"); BufferHandle handle = createBuffer(std::move(description)); buffers[handle.value].importedID = buffer; return handle; }
	PassHandle RenderGraph::addPass(RenderPassDescription description) { if (compiled || description.name.empty() || !description.execute) throw std::invalid_argument("Render pass requires a name and execute callback"); passes.push_back({ .description = std::move(description) }); return { static_cast<uint32>(passes.size() - 1) }; }

	void RenderGraph::validate() const
	{
		std::vector<bool> textureWritten(textures.size(), false); std::vector<bool> bufferWritten(buffers.size(), false);
		auto findFutureTextureProducer = [this](TextureHandle handle, uint32 afterPass) -> const std::string*
		{
			for (uint32 passIndex = afterPass + 1U; passIndex < passes.size(); ++passIndex)
			{
				const RenderPassDescription& candidate = passes[passIndex].description;
				if (std::any_of(candidate.writeTextures.begin(), candidate.writeTextures.end(), [handle](TextureHandle written) { return written.value == handle.value; })) return &candidate.name;
				if (std::any_of(candidate.colorAttachments.begin(), candidate.colorAttachments.end(), [handle](const TextureAttachment& attachment) { return attachment.texture.value == handle.value; })) return &candidate.name;
				if (candidate.depthAttachment && candidate.depthAttachment->texture.value == handle.value) return &candidate.name;
			}
			return nullptr;
		};
		auto findFutureBufferProducer = [this](BufferHandle handle, uint32 afterPass) -> const std::string*
		{
			for (uint32 passIndex = afterPass + 1U; passIndex < passes.size(); ++passIndex)
			{
				const RenderPassDescription& candidate = passes[passIndex].description;
				if (std::any_of(candidate.writeBuffers.begin(), candidate.writeBuffers.end(), [handle](BufferHandle written) { return written.value == handle.value; })) return &candidate.name;
			}
			return nullptr;
		};
		for (uint32 index = 0; index < passes.size(); ++index)
		{
			const RenderPassDescription& pass = passes[index].description;
			auto requireTexture = [&](TextureHandle handle, bool requireWrite) { if (!handle.isValid() || handle.value >= textures.size()) throw std::logic_error("Render pass references an invalid texture handle"); if (requireWrite && textures[handle.value].importedID == 0 && !textures[handle.value].description.persistent && !textureWritten[handle.value]) { if (const std::string* producer = findFutureTextureProducer(handle, index)) throw std::logic_error("Render graph has an unordered or cyclic texture dependency: pass '" + pass.name + "' reads " + textures[handle.value].description.debugName + " before producer '" + *producer + "'"); throw std::logic_error("Render graph texture is read before it is written: " + textures[handle.value].description.debugName); } };
			auto requireBuffer = [&](BufferHandle handle, bool requireWrite) { if (!handle.isValid() || handle.value >= buffers.size()) throw std::logic_error("Render pass references an invalid buffer handle"); if (requireWrite && buffers[handle.value].importedID == 0 && !buffers[handle.value].description.persistent && !bufferWritten[handle.value]) { if (const std::string* producer = findFutureBufferProducer(handle, index)) throw std::logic_error("Render graph has an unordered or cyclic buffer dependency: pass '" + pass.name + "' reads " + buffers[handle.value].description.debugName + " before producer '" + *producer + "'"); throw std::logic_error("Render graph buffer is read before it is written: " + buffers[handle.value].description.debugName); } };
			for (TextureHandle handle : pass.readTextures) requireTexture(handle, true);
			for (BufferHandle handle : pass.readBuffers) requireBuffer(handle, true);
			for (const TextureAttachment& attachment : pass.colorAttachments)
			{
				// A load operation is a true data dependency, not merely an FBO
				// binding.  It must have a prior producer unless the resource is
				// imported or carries persistent history from an earlier frame.
				if (attachment.load == LoadOperation::Load) requireTexture(attachment.texture, true);
				requireTexture(attachment.texture, false);
				textureWritten[attachment.texture.value] = true;
			}
			if (pass.depthAttachment)
			{
				if (pass.depthAttachment->load == LoadOperation::Load) requireTexture(pass.depthAttachment->texture, true);
				requireTexture(pass.depthAttachment->texture, false);
				textureWritten[pass.depthAttachment->texture.value] = true;
			}
			for (TextureHandle handle : pass.writeTextures) { requireTexture(handle, false); textureWritten[handle.value] = true; }
			for (BufferHandle handle : pass.writeBuffers) { requireBuffer(handle, false); bufferWritten[handle.value] = true; }
		}
	}

	void RenderGraph::allocateResources()
	{
		for (uint32 passIndex = 0; passIndex < passes.size(); ++passIndex)
		{
			auto markTexture = [&](TextureHandle handle) { TextureResource& resource = textures[handle.value]; resource.firstUse = std::min(resource.firstUse, passIndex); resource.lastUse = std::max(resource.lastUse, passIndex); };
			auto markBuffer = [&](BufferHandle handle) { BufferResource& resource = buffers[handle.value]; resource.firstUse = std::min(resource.firstUse, passIndex); resource.lastUse = std::max(resource.lastUse, passIndex); };
			const RenderPassDescription& pass = passes[passIndex].description;
			for (TextureHandle handle : pass.readTextures) markTexture(handle); for (const TextureAttachment& attachment : pass.colorAttachments) markTexture(attachment.texture); if (pass.depthAttachment) markTexture(pass.depthAttachment->texture); for (TextureHandle handle : pass.writeTextures) markTexture(handle);
			for (BufferHandle handle : pass.readBuffers) markBuffer(handle); for (BufferHandle handle : pass.writeBuffers) markBuffer(handle);
		}
		for (TextureResource& resource : textures)
		{
			if (resource.importedID != 0) continue;
			auto physical = std::find_if(texturePool.begin(), texturePool.end(), [&](const PhysicalTexture& candidate) { if (!sameTexture(candidate.description, resource.description)) return false; if (resource.description.persistent) return candidate.description.persistent; return !candidate.description.persistent && (candidate.lastUse == ~uint32 { 0 } || candidate.lastUse < resource.firstUse); });
			if (physical == texturePool.end())
			{
				PhysicalTexture created { .description = resource.description }; glCreateTextures(toGLTarget(created.description.dimension), 1, &created.texture);
				if (created.description.dimension == TextureDimension::Texture2D) glTextureStorage2D(created.texture, created.description.mipCount, toGLInternalFormat(created.description.format), created.description.extent.width, created.description.extent.height);
				else if (created.description.dimension == TextureDimension::Texture2DArray || created.description.dimension == TextureDimension::TextureCubeArray) glTextureStorage3D(created.texture, created.description.mipCount, toGLInternalFormat(created.description.format), created.description.extent.width, created.description.extent.height, created.description.layers);
				else if (created.description.dimension == TextureDimension::Texture2DMultisample) glTextureStorage2DMultisample(created.texture, static_cast<GLsizei>(created.description.sampleCount), toGLInternalFormat(created.description.format), static_cast<GLsizei>(created.description.extent.width), static_cast<GLsizei>(created.description.extent.height), GL_TRUE);
				else glTextureStorage3DMultisample(created.texture, static_cast<GLsizei>(created.description.sampleCount), toGLInternalFormat(created.description.format), static_cast<GLsizei>(created.description.extent.width), static_cast<GLsizei>(created.description.extent.height), static_cast<GLsizei>(created.description.layers), GL_TRUE);
				if (!isMultisampled(created.description.dimension)) { glTextureParameteri(created.texture, GL_TEXTURE_MIN_FILTER, created.description.mipCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR); glTextureParameteri(created.texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR); }
				glObjectLabel(GL_TEXTURE, created.texture, static_cast<GLsizei>(created.description.debugName.size()), created.description.debugName.c_str());
				texturePool.push_back(created); resource.physicalIndex = static_cast<uint32>(texturePool.size() - 1);
			}
			else { resource.physicalIndex = static_cast<uint32>(physical - texturePool.begin()); }
			texturePool[resource.physicalIndex].lastUse = resource.lastUse;
		}
		for (BufferResource& resource : buffers)
		{
			if (resource.importedID != 0) continue;
			auto physical = std::find_if(bufferPool.begin(), bufferPool.end(), [&](const PhysicalBuffer& candidate) { if (candidate.description.sizeInBytes != resource.description.sizeInBytes || candidate.description.storageFlags != resource.description.storageFlags) return false; if (resource.description.persistent) return candidate.description.persistent; return !candidate.description.persistent && (candidate.lastUse == ~uint32 { 0 } || candidate.lastUse < resource.firstUse); });
			if (physical == bufferPool.end()) { PhysicalBuffer created { .description = resource.description }; glCreateBuffers(1, &created.buffer); glNamedBufferStorage(created.buffer, static_cast<GLsizeiptr>(created.description.sizeInBytes), nullptr, created.description.storageFlags); glObjectLabel(GL_BUFFER, created.buffer, static_cast<GLsizei>(created.description.debugName.size()), created.description.debugName.c_str()); bufferPool.push_back(created); resource.physicalIndex = static_cast<uint32>(bufferPool.size() - 1); }
			else resource.physicalIndex = static_cast<uint32>(physical - bufferPool.begin());
			bufferPool[resource.physicalIndex].lastUse = resource.lastUse;
		}
	}

	void RenderGraph::createPassFramebuffers()
	{
		for (PassResource& pass : passes)
		{
			if (pass.description.queue != PassQueue::Graphics || (pass.description.colorAttachments.empty() && !pass.description.depthAttachment)) continue;
			glCreateFramebuffers(1, &pass.framebuffer); glObjectLabel(GL_FRAMEBUFFER, pass.framebuffer, static_cast<GLsizei>(pass.description.name.size()), pass.description.name.c_str()); std::vector<GLenum> drawBuffers;
			for (uint32 colorIndex = 0; colorIndex < pass.description.colorAttachments.size(); ++colorIndex) { glNamedFramebufferTexture(pass.framebuffer, GL_COLOR_ATTACHMENT0 + colorIndex, getTextureResource(pass.description.colorAttachments[colorIndex].texture).importedID != 0 ? getTextureResource(pass.description.colorAttachments[colorIndex].texture).importedID : texturePool[getTextureResource(pass.description.colorAttachments[colorIndex].texture).physicalIndex].texture, 0); drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + colorIndex); }
			if (pass.description.depthAttachment) { const TextureResource& depth = getTextureResource(pass.description.depthAttachment->texture); glNamedFramebufferTexture(pass.framebuffer, GL_DEPTH_ATTACHMENT, depth.importedID != 0 ? depth.importedID : texturePool[depth.physicalIndex].texture, 0); }
			if (!drawBuffers.empty()) glNamedFramebufferDrawBuffers(pass.framebuffer, static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data()); else glNamedFramebufferDrawBuffer(pass.framebuffer, GL_NONE);
			if (glCheckNamedFramebufferStatus(pass.framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("Render graph created an incomplete framebuffer for pass " + pass.description.name);
		}
	}
	void RenderGraph::compile() { if (compiled) return; pipeline::device::requireOpenGL46Context(); validate(); allocateResources(); createPassFramebuffers(); compiled = true; }
	void RenderGraph::execute()
	{
		if (!compiled) throw std::logic_error("Render graph must be compiled before execution");
		for (uint32 index = 0; index < passes.size(); ++index) { PassResource& pass = passes[index]; RenderGraphContext context(*this, { index }); if (pass.description.queue == PassQueue::Graphics) { context.bindPassFramebuffer(); Extent2D viewportExtent = frameExtent; if (!pass.description.colorAttachments.empty()) viewportExtent = getTextureResource(pass.description.colorAttachments.front().texture).description.extent; else if (pass.description.depthAttachment) viewportExtent = getTextureResource(pass.description.depthAttachment->texture).description.extent; else if (!pass.description.readTextures.empty()) viewportExtent = getTextureResource(pass.description.readTextures.front()).description.extent; glViewport(0, 0, static_cast<GLsizei>(viewportExtent.width), static_cast<GLsizei>(viewportExtent.height)); invalidateDiscardedAttachments(pass, true); for (uint32 attachmentIndex = 0; attachmentIndex < pass.description.colorAttachments.size(); ++attachmentIndex) { const TextureAttachment& attachment = pass.description.colorAttachments[attachmentIndex]; if (attachment.load == LoadOperation::Clear) { const TextureFormat format = getTextureResource(attachment.texture).description.format; if (format == TextureFormat::R32UnsignedInteger) { const glm::uvec4 clearValue { static_cast<uint32>(attachment.clearColor.x), static_cast<uint32>(attachment.clearColor.y), static_cast<uint32>(attachment.clearColor.z), static_cast<uint32>(attachment.clearColor.w) }; glClearNamedFramebufferuiv(pass.framebuffer, GL_COLOR, attachmentIndex, &clearValue[0]); } else { glm::vec4 clearColor = attachment.clearColor; glClearNamedFramebufferfv(pass.framebuffer, GL_COLOR, attachmentIndex, &clearColor[0]); } } } if (pass.description.depthAttachment && pass.description.depthAttachment->load == LoadOperation::Clear) { float32 clearDepth = pass.description.depthAttachment->clearDepth; glClearNamedFramebufferfv(pass.framebuffer, GL_DEPTH, 0, &clearDepth); } } glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, index, static_cast<GLsizei>(pass.description.name.size()), pass.description.name.c_str()); try { pass.description.execute(context); } catch (...) { glPopDebugGroup(); throw; } glPopDebugGroup(); if (pass.description.queue == PassQueue::Graphics) invalidateDiscardedAttachments(pass, false); pipeline::device::throwPendingOpenGLErrors("Render graph pass '" + pass.description.name + "'"); }
	}
	void RenderGraph::releasePassFramebuffers() noexcept { for (PassResource& pass : passes) if (pass.framebuffer != 0) glDeleteFramebuffers(1, &pass.framebuffer); }
	void RenderGraph::reset() { releasePassFramebuffers(); textures.clear(); buffers.clear(); passes.clear(); compiled = false; }
	bool RenderGraph::isCompiled() const noexcept { return compiled; }
	const RenderGraph::TextureResource& RenderGraph::getTextureResource(TextureHandle handle) const { if (!handle.isValid() || handle.value >= textures.size()) throw std::out_of_range("Invalid render graph texture handle"); return textures[handle.value]; }
	const RenderGraph::BufferResource& RenderGraph::getBufferResource(BufferHandle handle) const { if (!handle.isValid() || handle.value >= buffers.size()) throw std::out_of_range("Invalid render graph buffer handle"); return buffers[handle.value]; }
	const RenderGraph::PassResource& RenderGraph::getPassResource(PassHandle handle) const { if (!handle.isValid() || handle.value >= passes.size()) throw std::out_of_range("Invalid render graph pass handle"); return passes[handle.value]; }
	GLuint RenderGraphContext::getTexture(TextureHandle handle) const { const RenderGraph::TextureResource& resource = graph.getTextureResource(handle); return resource.importedID != 0 ? resource.importedID : graph.texturePool[resource.physicalIndex].texture; }
	GLuint RenderGraphContext::getBuffer(BufferHandle handle) const { const RenderGraph::BufferResource& resource = graph.getBufferResource(handle); return resource.importedID != 0 ? resource.importedID : graph.bufferPool[resource.physicalIndex].buffer; }
	Extent2D RenderGraphContext::getExtent(TextureHandle handle) const { return graph.getTextureResource(handle).description.extent; }
	void RenderGraphContext::bindPassFramebuffer() const { glBindFramebuffer(GL_FRAMEBUFFER, graph.getPassResource(pass).framebuffer); }
	void RenderGraphContext::validateGraphicsPipelineTargets(const pipeline::shader::GraphicsPipeline& pipeline) const
	{
		const RenderGraph::PassResource& currentPass = graph.getPassResource(pass);
		if (currentPass.description.queue != PassQueue::Graphics) throw std::logic_error("Only a graphics render-graph pass can bind a graphics pipeline");
		const pipeline::shader::RenderTargetSignature& expected = pipeline.getDescriptor().state.renderTargets;
		if (expected.colorAttachmentCount != currentPass.description.colorAttachments.size() || expected.hasDepth != currentPass.description.depthAttachment.has_value()) throw std::logic_error("Graphics pipeline render-target signature does not match render-graph pass '" + currentPass.description.name + "'");
		uint8 actualSampleCount = 1;
		bool sampleCountInitialized = false;
		auto requireSampleCount = [this, &actualSampleCount, &sampleCountInitialized](TextureHandle handle)
		{
			const uint8 sampleCount = graph.getTextureResource(handle).description.sampleCount;
			if (!sampleCountInitialized) { actualSampleCount = sampleCount; sampleCountInitialized = true; }
			else if (actualSampleCount != sampleCount) throw std::logic_error("Render graph pass attachments use incompatible sample counts");
		};
		for (const TextureAttachment& attachment : currentPass.description.colorAttachments) requireSampleCount(attachment.texture);
		if (currentPass.description.depthAttachment) requireSampleCount(currentPass.description.depthAttachment->texture);
		if (sampleCountInitialized && expected.sampleCount != actualSampleCount) throw std::logic_error("Graphics pipeline sample count does not match render-graph attachments for pass '" + currentPass.description.name + "'");
	}
}
