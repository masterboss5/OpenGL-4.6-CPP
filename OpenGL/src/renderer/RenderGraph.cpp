#include "RenderGraph.h"

#include "src/pipeline/device/Device.h"
#include "src/pipeline/shader/GraphicsPipeline.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace renderer::graph
{
struct RenderGraph::TextureResource final
{
	TextureDescription Description;
	GLuint ImportedID = 0;
	uint32 PhysicalIndex = ~uint32{0};
	uint32 FirstUse = ~uint32{0};
	uint32 LastUse = 0;
};
struct RenderGraph::BufferResource final
{
	BufferDescription Description;
	GLuint ImportedID = 0;
	uint32 PhysicalIndex = ~uint32{0};
	uint32 FirstUse = ~uint32{0};
	uint32 LastUse = 0;
};
struct RenderGraph::PassResource final
{
	RenderPassDescription Description;
	GLuint Framebuffer = 0;
};
struct RenderGraph::PhysicalTexture final
{
	TextureDescription Description;
	GLuint Texture = 0;
	uint32 LastUse = 0;
	uint64 LastFrameUsed = 0;
};
struct RenderGraph::PhysicalBuffer final
{
	BufferDescription Description;
	GLuint Buffer = 0;
	uint32 LastUse = 0;
	uint64 LastFrameUsed = 0;
};

RenderGraph::RenderGraph(pipeline::device::Device &Device) : Device(&Device)
{
}

namespace
{
[[nodiscard]] GLenum ToGlInternalFormat(TextureFormat Format)
{
	switch (Format)
	{
	case TextureFormat::Depth32Float:
		return GL_DEPTH_COMPONENT32F;
	case TextureFormat::RGBA16Float:
		return GL_RGBA16F;
	case TextureFormat::RG16Float:
		return GL_RG16F;
	case TextureFormat::R32UnsignedInteger:
		return GL_R32UI;
	case TextureFormat::R32Float:
		return GL_R32F;
	case TextureFormat::R8Unorm:
		return GL_R8;
	}
	throw std::logic_error("Unknown render graph texture format");
}
[[nodiscard]] pipeline::device::DeviceFormat ToDeviceFormat(TextureFormat Format)
{
	switch (Format)
	{
	case TextureFormat::Depth32Float:
		return pipeline::device::DeviceFormat::Depth32Float;
	case TextureFormat::RGBA16Float:
		return pipeline::device::DeviceFormat::RGBA16Float;
	case TextureFormat::RG16Float:
		return pipeline::device::DeviceFormat::RG16Float;
	case TextureFormat::R32UnsignedInteger:
		return pipeline::device::DeviceFormat::R32UnsignedInteger;
	case TextureFormat::R32Float:
		return pipeline::device::DeviceFormat::R32Float;
	case TextureFormat::R8Unorm:
		return pipeline::device::DeviceFormat::R8Unorm;
	}
	throw std::logic_error("Unknown render graph texture format");
}
[[nodiscard]] GLenum ToGlTarget(TextureDimension Dimension)
{
	switch (Dimension)
	{
	case TextureDimension::Texture2D:
		return GL_TEXTURE_2D;
	case TextureDimension::Texture2DArray:
		return GL_TEXTURE_2D_ARRAY;
	case TextureDimension::TextureCubeArray:
		return GL_TEXTURE_CUBE_MAP_ARRAY;
	case TextureDimension::Texture2DMultisample:
		return GL_TEXTURE_2D_MULTISAMPLE;
	case TextureDimension::Texture2DMultisampleArray:
		return GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
	}
	throw std::logic_error("Unknown render graph texture dimension");
}
[[nodiscard]] bool IsMultisampled(TextureDimension Dimension)
{
	return Dimension == TextureDimension::Texture2DMultisample || Dimension == TextureDimension::Texture2DMultisampleArray;
}
[[nodiscard]] bool SameTexture(const TextureDescription &Left, const TextureDescription &Right)
{
	return Left.Extent.Width == Right.Extent.Width && Left.Extent.Height == Right.Extent.Height && Left.Format == Right.Format &&
		   Left.Dimension == Right.Dimension && Left.MipCount == Right.MipCount && Left.Layers == Right.Layers &&
		   Left.SampleCount == Right.SampleCount;
}

template <typename PassResourceType> void InvalidateDiscardedAttachments(const PassResourceType &Pass, bool BeforeExecution)
{
	if (Pass.Framebuffer == 0)
		return;
	std::vector<GLenum> Attachments;
	for (uint32 ColorIndex = 0; ColorIndex < Pass.Description.ColorAttachments.size(); ++ColorIndex)
	{
		const TextureAttachment &Attachment = Pass.Description.ColorAttachments[ColorIndex];
		const bool Discard = BeforeExecution ? Attachment.Load == LoadOperation::Discard : Attachment.Store == StoreOperation::Discard;
		if (Discard)
			Attachments.push_back(GL_COLOR_ATTACHMENT0 + ColorIndex);
	}
	if (Pass.Description.DepthAttachment)
	{
		const DepthAttachment &Attachment = *Pass.Description.DepthAttachment;
		const bool Discard = BeforeExecution ? Attachment.Load == LoadOperation::Discard : Attachment.Store == StoreOperation::Discard;
		if (Discard)
			Attachments.push_back(GL_DEPTH_ATTACHMENT);
	}
	if (!Attachments.empty())
		glInvalidateNamedFramebufferData(Pass.Framebuffer, static_cast<GLsizei>(Attachments.size()), Attachments.data());
}
} // namespace

RenderGraph::~RenderGraph()
{
	if (this->Device != nullptr && this->Device->CanIssueCommands())
	{
		this->ReleasePassFramebuffers();
		for (PhysicalTexture &Texture : TexturePool)
			if (Texture.Texture != 0)
				glDeleteTextures(1, &Texture.Texture);
		for (PhysicalBuffer &Buffer : BufferPool)
			if (Buffer.Buffer != 0)
				glDeleteBuffers(1, &Buffer.Buffer);
	}
}
void RenderGraph::BeginFrame(Extent2D Extent)
{
	if (!Extent.IsValid())
		throw std::invalid_argument("Render graph requires a non-zero frame extent");
	this->Reset();
	++this->FrameSerial;
	if (this->FrameSerial == 0)
		this->FrameSerial = 1;
	constexpr uint64 RetainedFrameCount = 3;
	std::erase_if(this->TexturePool,
				  [this](PhysicalTexture &Texture)
				  {
					  if (this->FrameSerial - Texture.LastFrameUsed <= RetainedFrameCount)
						  return false;
					  if (Texture.Texture != 0)
						  glDeleteTextures(1, &Texture.Texture);
					  return true;
				  });
	std::erase_if(this->BufferPool,
				  [this](PhysicalBuffer &Buffer)
				  {
					  if (this->FrameSerial - Buffer.LastFrameUsed <= RetainedFrameCount)
						  return false;
					  if (Buffer.Buffer != 0)
						  glDeleteBuffers(1, &Buffer.Buffer);
					  return true;
				  });
	this->FrameExtent = Extent;
	for (PhysicalTexture &Texture : TexturePool)
		Texture.LastUse = ~uint32{0};
	for (PhysicalBuffer &Buffer : BufferPool)
		Buffer.LastUse = ~uint32{0};
}
TextureHandle RenderGraph::CreateTexture(TextureDescription Description)
{
	if (Compiled)
		throw std::logic_error("Cannot create render graph resources after compile");
	if (!Description.Extent.IsValid())
		Description.Extent = FrameExtent;
	const bool Multisampled = IsMultisampled(Description.Dimension);
	if (!Description.Extent.IsValid() || Description.MipCount == 0 || Description.Layers == 0 || Description.SampleCount == 0 ||
		(Multisampled && (Description.SampleCount == 1 || Description.MipCount != 1)) || (!Multisampled && Description.SampleCount != 1))
		throw std::invalid_argument("Invalid render graph texture description");
	if ((Description.Dimension == TextureDimension::Texture2D || Description.Dimension == TextureDimension::Texture2DMultisample) &&
		Description.Layers != 1)
		throw std::invalid_argument("Non-array render graph textures require exactly one layer");
	if (Description.Dimension == TextureDimension::TextureCubeArray && Description.Layers % 6 != 0)
		throw std::invalid_argument("Cube-array render graph texture layers must be a multiple of six");
	const pipeline::device::DeviceCapabilities &Limits = this->Device->GetCapabilities();
	const uint32 MaximumDimension =
		Description.Dimension == TextureDimension::TextureCubeArray ? Limits.MaximumCubeMapTextureSize : Limits.MaximumTextureSize;
	if (Description.Extent.Width > MaximumDimension || Description.Extent.Height > MaximumDimension)
		throw std::invalid_argument("Render graph texture extent exceeds Device limits");
	if ((Description.Dimension == TextureDimension::Texture2DArray ||
		 Description.Dimension == TextureDimension::Texture2DMultisampleArray ||
		 Description.Dimension == TextureDimension::TextureCubeArray) &&
		Description.Layers > Limits.MaximumTextureArrayLayers)
		throw std::invalid_argument("Render graph texture layer count exceeds Device limits");
	const uint32 MaximumMipCount = static_cast<uint32>(std::bit_width(std::max(Description.Extent.Width, Description.Extent.Height)));
	if (Description.MipCount > MaximumMipCount)
		throw std::invalid_argument("Render graph texture mip count exceeds its complete mip chain");
	const pipeline::device::DeviceFormatCapabilities &Format = this->Device->GetFormatCapabilities(ToDeviceFormat(Description.Format));
	if (!Format.Supported)
		throw std::invalid_argument("Render graph texture format is unsupported by the Device");
	if (Multisampled &&
		std::find(Format.SampleCounts.begin(), Format.SampleCounts.end(), Description.SampleCount) == Format.SampleCounts.end())
		throw std::invalid_argument("Render graph texture sample count is unsupported for its format");
	Textures.push_back({.Description = std::move(Description)});
	return {static_cast<uint32>(Textures.size() - 1)};
}
TextureHandle RenderGraph::ImportTexture(TextureDescription Description, GLuint Texture)
{
	if (Texture == 0)
		throw std::invalid_argument("Cannot import OpenGL texture 0");
	TextureHandle Handle = CreateTexture(std::move(Description));
	Textures[Handle.Value].ImportedID = Texture;
	return Handle;
}
BufferHandle RenderGraph::CreateBuffer(BufferDescription Description)
{
	if (Compiled || Description.SizeInBytes == 0)
		throw std::invalid_argument("Invalid render graph buffer description");
	Buffers.push_back({.Description = std::move(Description)});
	return {static_cast<uint32>(Buffers.size() - 1)};
}
BufferHandle RenderGraph::ImportBuffer(BufferDescription Description, GLuint Buffer)
{
	if (Buffer == 0)
		throw std::invalid_argument("Cannot import OpenGL buffer 0");
	BufferHandle Handle = CreateBuffer(std::move(Description));
	Buffers[Handle.Value].ImportedID = Buffer;
	return Handle;
}
PassHandle RenderGraph::AddPass(RenderPassDescription Description)
{
	if (Compiled || Description.Name.empty() || !Description.Execute)
		throw std::invalid_argument("Render pass requires a name and execute callback");
	Passes.push_back({.Description = std::move(Description)});
	return {static_cast<uint32>(Passes.size() - 1)};
}

void RenderGraph::Validate() const
{
	std::vector<bool> TextureWritten(Textures.size(), false);
	std::vector<bool> BufferWritten(Buffers.size(), false);
	auto FindFutureTextureProducer = [this](TextureHandle Handle, uint32 AfterPass) -> const std::string *
	{
		for (uint32 PassIndex = AfterPass + 1U; PassIndex < Passes.size(); ++PassIndex)
		{
			const RenderPassDescription &Candidate = Passes[PassIndex].Description;
			if (std::any_of(Candidate.WriteTextures.begin(), Candidate.WriteTextures.end(),
							[Handle](TextureHandle Written) { return Written.Value == Handle.Value; }))
				return &Candidate.Name;
			if (std::any_of(Candidate.ColorAttachments.begin(), Candidate.ColorAttachments.end(),
							[Handle](const TextureAttachment &Attachment) { return Attachment.Texture.Value == Handle.Value; }))
				return &Candidate.Name;
			if (Candidate.DepthAttachment && Candidate.DepthAttachment->Texture.Value == Handle.Value)
				return &Candidate.Name;
		}
		return nullptr;
	};
	auto FindFutureBufferProducer = [this](BufferHandle Handle, uint32 AfterPass) -> const std::string *
	{
		for (uint32 PassIndex = AfterPass + 1U; PassIndex < Passes.size(); ++PassIndex)
		{
			const RenderPassDescription &Candidate = Passes[PassIndex].Description;
			if (std::any_of(Candidate.WriteBuffers.begin(), Candidate.WriteBuffers.end(),
							[Handle](BufferHandle Written) { return Written.Value == Handle.Value; }))
				return &Candidate.Name;
		}
		return nullptr;
	};
	for (uint32 Index = 0; Index < Passes.size(); ++Index)
	{
		const RenderPassDescription &Pass = Passes[Index].Description;
		auto RequireTexture = [&](TextureHandle Handle, bool RequireWrite)
		{
			if (!Handle.IsValid() || Handle.Value >= Textures.size())
				throw std::logic_error("Render pass references an invalid texture handle");
			if (RequireWrite && Textures[Handle.Value].ImportedID == 0 && !Textures[Handle.Value].Description.Persistent &&
				!TextureWritten[Handle.Value])
			{
				if (const std::string *Producer = FindFutureTextureProducer(Handle, Index))
					throw std::logic_error("Render graph has an unordered or cyclic texture dependency: pass '" + Pass.Name + "' reads " +
										   Textures[Handle.Value].Description.DebugName + " before producer '" + *Producer + "'");
				throw std::logic_error("Render graph texture is read before it is written: " +
									   Textures[Handle.Value].Description.DebugName);
			}
		};
		auto RequireBuffer = [&](BufferHandle Handle, bool RequireWrite)
		{
			if (!Handle.IsValid() || Handle.Value >= Buffers.size())
				throw std::logic_error("Render pass references an invalid buffer handle");
			if (RequireWrite && Buffers[Handle.Value].ImportedID == 0 && !Buffers[Handle.Value].Description.Persistent &&
				!BufferWritten[Handle.Value])
			{
				if (const std::string *Producer = FindFutureBufferProducer(Handle, Index))
					throw std::logic_error("Render graph has an unordered or cyclic buffer dependency: pass '" + Pass.Name + "' reads " +
										   Buffers[Handle.Value].Description.DebugName + " before producer '" + *Producer + "'");
				throw std::logic_error("Render graph buffer is read before it is written: " + Buffers[Handle.Value].Description.DebugName);
			}
		};
		for (TextureHandle Handle : Pass.ReadTextures)
			RequireTexture(Handle, true);
		for (BufferHandle Handle : Pass.ReadBuffers)
			RequireBuffer(Handle, true);
		for (const TextureAttachment &Attachment : Pass.ColorAttachments)
		{
			// A load operation is a true data dependency, not merely an FBO
			// binding.  It must have a prior producer unless the resource is
			// imported or carries persistent history from an earlier frame.
			if (Attachment.Load == LoadOperation::Load)
				RequireTexture(Attachment.Texture, true);
			RequireTexture(Attachment.Texture, false);
			if (!this->Device->GetFormatCapabilities(ToDeviceFormat(Textures[Attachment.Texture.Value].Description.Format)).ColorRenderable)
				throw std::logic_error("Render graph color attachment format is not color-renderable in pass '" + Pass.Name + "'");
			TextureWritten[Attachment.Texture.Value] = true;
		}
		if (Pass.DepthAttachment)
		{
			if (Pass.DepthAttachment->Load == LoadOperation::Load)
				RequireTexture(Pass.DepthAttachment->Texture, true);
			RequireTexture(Pass.DepthAttachment->Texture, false);
			if (!this->Device->GetFormatCapabilities(ToDeviceFormat(Textures[Pass.DepthAttachment->Texture.Value].Description.Format))
					 .DepthRenderable)
				throw std::logic_error("Render graph depth attachment format is not depth-renderable in pass '" + Pass.Name + "'");
			TextureWritten[Pass.DepthAttachment->Texture.Value] = true;
		}
		for (TextureHandle Handle : Pass.WriteTextures)
		{
			RequireTexture(Handle, false);
			if (!this->Device->GetFormatCapabilities(ToDeviceFormat(Textures[Handle.Value].Description.Format)).ShaderImageStore)
				throw std::logic_error("Render graph write texture format does not support shader image stores in pass '" + Pass.Name +
									   "'");
			TextureWritten[Handle.Value] = true;
		}
		for (BufferHandle Handle : Pass.WriteBuffers)
		{
			RequireBuffer(Handle, false);
			BufferWritten[Handle.Value] = true;
		}
	}
}

void RenderGraph::AllocateResources()
{
	for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
	{
		auto MarkTexture = [&](TextureHandle Handle)
		{
			TextureResource &Resource = Textures[Handle.Value];
			Resource.FirstUse = std::min(Resource.FirstUse, PassIndex);
			Resource.LastUse = std::max(Resource.LastUse, PassIndex);
		};
		auto MarkBuffer = [&](BufferHandle Handle)
		{
			BufferResource &Resource = Buffers[Handle.Value];
			Resource.FirstUse = std::min(Resource.FirstUse, PassIndex);
			Resource.LastUse = std::max(Resource.LastUse, PassIndex);
		};
		const RenderPassDescription &Pass = Passes[PassIndex].Description;
		for (TextureHandle Handle : Pass.ReadTextures)
			MarkTexture(Handle);
		for (const TextureAttachment &Attachment : Pass.ColorAttachments)
			MarkTexture(Attachment.Texture);
		if (Pass.DepthAttachment)
			MarkTexture(Pass.DepthAttachment->Texture);
		for (TextureHandle Handle : Pass.WriteTextures)
			MarkTexture(Handle);
		for (BufferHandle Handle : Pass.ReadBuffers)
			MarkBuffer(Handle);
		for (BufferHandle Handle : Pass.WriteBuffers)
			MarkBuffer(Handle);
	}
	for (TextureResource &Resource : Textures)
	{
		if (Resource.ImportedID != 0)
			continue;
		auto Physical = std::find_if(
			TexturePool.begin(), TexturePool.end(),
			[&](const PhysicalTexture &Candidate)
			{
				if (!SameTexture(Candidate.Description, Resource.Description))
					return false;
				if (Resource.Description.Persistent)
					return Candidate.Description.Persistent && Candidate.Description.DebugName == Resource.Description.DebugName;
				return !Candidate.Description.Persistent && (Candidate.LastUse == ~uint32{0} || Candidate.LastUse < Resource.FirstUse);
			});
		if (Physical == TexturePool.end())
		{
			PhysicalTexture Created{.Description = Resource.Description};
			glCreateTextures(ToGlTarget(Created.Description.Dimension), 1, &Created.Texture);
			if (Created.Description.Dimension == TextureDimension::Texture2D)
				glTextureStorage2D(Created.Texture, Created.Description.MipCount, ToGlInternalFormat(Created.Description.Format),
								   Created.Description.Extent.Width, Created.Description.Extent.Height);
			else if (Created.Description.Dimension == TextureDimension::Texture2DArray ||
					 Created.Description.Dimension == TextureDimension::TextureCubeArray)
				glTextureStorage3D(Created.Texture, Created.Description.MipCount, ToGlInternalFormat(Created.Description.Format),
								   Created.Description.Extent.Width, Created.Description.Extent.Height, Created.Description.Layers);
			else if (Created.Description.Dimension == TextureDimension::Texture2DMultisample)
				glTextureStorage2DMultisample(Created.Texture, static_cast<GLsizei>(Created.Description.SampleCount),
											  ToGlInternalFormat(Created.Description.Format),
											  static_cast<GLsizei>(Created.Description.Extent.Width),
											  static_cast<GLsizei>(Created.Description.Extent.Height), GL_TRUE);
			else
				glTextureStorage3DMultisample(
					Created.Texture, static_cast<GLsizei>(Created.Description.SampleCount), ToGlInternalFormat(Created.Description.Format),
					static_cast<GLsizei>(Created.Description.Extent.Width), static_cast<GLsizei>(Created.Description.Extent.Height),
					static_cast<GLsizei>(Created.Description.Layers), GL_TRUE);
			if (!IsMultisampled(Created.Description.Dimension))
			{
				const bool Filterable = this->Device->GetFormatCapabilities(ToDeviceFormat(Created.Description.Format)).Filterable;
				glTextureParameteri(Created.Texture, GL_TEXTURE_MIN_FILTER,
									Filterable ? (Created.Description.MipCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR) : GL_NEAREST);
				glTextureParameteri(Created.Texture, GL_TEXTURE_MAG_FILTER, Filterable ? GL_LINEAR : GL_NEAREST);
			}
			glObjectLabel(GL_TEXTURE, Created.Texture, static_cast<GLsizei>(Created.Description.DebugName.size()),
						  Created.Description.DebugName.c_str());
			TexturePool.push_back(Created);
			Resource.PhysicalIndex = static_cast<uint32>(TexturePool.size() - 1);
		}
		else
		{
			Resource.PhysicalIndex = static_cast<uint32>(Physical - TexturePool.begin());
		}
		TexturePool[Resource.PhysicalIndex].LastUse = Resource.LastUse;
		TexturePool[Resource.PhysicalIndex].LastFrameUsed = this->FrameSerial;
	}
	for (BufferResource &Resource : Buffers)
	{
		if (Resource.ImportedID != 0)
			continue;
		auto Physical = std::find_if(
			BufferPool.begin(), BufferPool.end(),
			[&](const PhysicalBuffer &Candidate)
			{
				if (Candidate.Description.SizeInBytes != Resource.Description.SizeInBytes ||
					Candidate.Description.StorageFlags != Resource.Description.StorageFlags)
					return false;
				if (Resource.Description.Persistent)
					return Candidate.Description.Persistent && Candidate.Description.DebugName == Resource.Description.DebugName;
				return !Candidate.Description.Persistent && (Candidate.LastUse == ~uint32{0} || Candidate.LastUse < Resource.FirstUse);
			});
		if (Physical == BufferPool.end())
		{
			PhysicalBuffer Created{.Description = Resource.Description};
			glCreateBuffers(1, &Created.Buffer);
			glNamedBufferStorage(Created.Buffer, static_cast<GLsizeiptr>(Created.Description.SizeInBytes), nullptr,
								 Created.Description.StorageFlags);
			glObjectLabel(GL_BUFFER, Created.Buffer, static_cast<GLsizei>(Created.Description.DebugName.size()),
						  Created.Description.DebugName.c_str());
			BufferPool.push_back(Created);
			Resource.PhysicalIndex = static_cast<uint32>(BufferPool.size() - 1);
		}
		else
			Resource.PhysicalIndex = static_cast<uint32>(Physical - BufferPool.begin());
		BufferPool[Resource.PhysicalIndex].LastUse = Resource.LastUse;
		BufferPool[Resource.PhysicalIndex].LastFrameUsed = this->FrameSerial;
	}
}

void RenderGraph::CreatePassFramebuffers()
{
	for (PassResource &Pass : Passes)
	{
		if (Pass.Description.Queue != PassQueue::Graphics ||
			(Pass.Description.ColorAttachments.empty() && !Pass.Description.DepthAttachment))
			continue;
		glCreateFramebuffers(1, &Pass.Framebuffer);
		glObjectLabel(GL_FRAMEBUFFER, Pass.Framebuffer, static_cast<GLsizei>(Pass.Description.Name.size()), Pass.Description.Name.c_str());
		std::vector<GLenum> DrawBuffers;
		for (uint32 ColorIndex = 0; ColorIndex < Pass.Description.ColorAttachments.size(); ++ColorIndex)
		{
			glNamedFramebufferTexture(
				Pass.Framebuffer, GL_COLOR_ATTACHMENT0 + ColorIndex,
				GetTextureResource(Pass.Description.ColorAttachments[ColorIndex].Texture).ImportedID != 0
					? GetTextureResource(Pass.Description.ColorAttachments[ColorIndex].Texture).ImportedID
					: TexturePool[GetTextureResource(Pass.Description.ColorAttachments[ColorIndex].Texture).PhysicalIndex].Texture,
				0);
			DrawBuffers.push_back(GL_COLOR_ATTACHMENT0 + ColorIndex);
		}
		if (Pass.Description.DepthAttachment)
		{
			const TextureResource &Depth = GetTextureResource(Pass.Description.DepthAttachment->Texture);
			glNamedFramebufferTexture(Pass.Framebuffer, GL_DEPTH_ATTACHMENT,
									  Depth.ImportedID != 0 ? Depth.ImportedID : TexturePool[Depth.PhysicalIndex].Texture, 0);
		}
		if (!DrawBuffers.empty())
			glNamedFramebufferDrawBuffers(Pass.Framebuffer, static_cast<GLsizei>(DrawBuffers.size()), DrawBuffers.data());
		else
			glNamedFramebufferDrawBuffer(Pass.Framebuffer, GL_NONE);
		if (glCheckNamedFramebufferStatus(Pass.Framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			throw std::runtime_error("Render graph created an incomplete framebuffer for pass " + Pass.Description.Name);
	}
}
void RenderGraph::Compile()
{
	if (Compiled)
		return;
	(void)this->Device->RequireCurrentContext();
	Validate();
	AllocateResources();
	CreatePassFramebuffers();
	this->Device->CheckErrors("RenderGraph compilation");
	Compiled = true;
}
void RenderGraph::Execute()
{
	if (!Compiled)
		throw std::logic_error("Render graph must be compiled before execution");
	for (uint32 Index = 0; Index < Passes.size(); ++Index)
	{
		if (Index != 0)
		{
			const RenderPassDescription &Previous = Passes[Index - 1U].Description;
			GLbitfield Barriers = 0;
			if (!Previous.WriteTextures.empty())
				Barriers |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT;
			if (!Previous.ColorAttachments.empty() || Previous.DepthAttachment.has_value())
				Barriers |= GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
			if (!Previous.WriteBuffers.empty())
				Barriers |= GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT;
			if (Barriers != 0)
				glMemoryBarrier(Barriers);
		}
		PassResource &Pass = Passes[Index];
		RenderGraphContext Context(*this, {Index});
		if (Pass.Description.Queue == PassQueue::Graphics)
		{
			Context.BindPassFramebuffer();
			Extent2D ViewportExtent = FrameExtent;
			if (!Pass.Description.ColorAttachments.empty())
				ViewportExtent = GetTextureResource(Pass.Description.ColorAttachments.front().Texture).Description.Extent;
			else if (Pass.Description.DepthAttachment)
				ViewportExtent = GetTextureResource(Pass.Description.DepthAttachment->Texture).Description.Extent;
			else if (!Pass.Description.ReadTextures.empty())
				ViewportExtent = GetTextureResource(Pass.Description.ReadTextures.front()).Description.Extent;
			glViewport(0, 0, static_cast<GLsizei>(ViewportExtent.Width), static_cast<GLsizei>(ViewportExtent.Height));
			InvalidateDiscardedAttachments(Pass, true);
			for (uint32 AttachmentIndex = 0; AttachmentIndex < Pass.Description.ColorAttachments.size(); ++AttachmentIndex)
			{
				const TextureAttachment &Attachment = Pass.Description.ColorAttachments[AttachmentIndex];
				if (Attachment.Load == LoadOperation::Clear)
				{
					const TextureFormat Format = GetTextureResource(Attachment.Texture).Description.Format;
					if (Format == TextureFormat::R32UnsignedInteger)
					{
						const glm::uvec4 ClearValue{
							static_cast<uint32>(Attachment.ClearColor.x), static_cast<uint32>(Attachment.ClearColor.y),
							static_cast<uint32>(Attachment.ClearColor.z), static_cast<uint32>(Attachment.ClearColor.w)};
						glClearNamedFramebufferuiv(Pass.Framebuffer, GL_COLOR, AttachmentIndex, &ClearValue[0]);
					}
					else
					{
						glm::vec4 ClearColor = Attachment.ClearColor;
						glClearNamedFramebufferfv(Pass.Framebuffer, GL_COLOR, AttachmentIndex, &ClearColor[0]);
					}
				}
			}
			if (Pass.Description.DepthAttachment && Pass.Description.DepthAttachment->Load == LoadOperation::Clear)
			{
				float32 ClearDepth = Pass.Description.DepthAttachment->ClearDepth;
				glClearNamedFramebufferfv(Pass.Framebuffer, GL_DEPTH, 0, &ClearDepth);
			}
		}
		glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, Index, static_cast<GLsizei>(Pass.Description.Name.size()),
						 Pass.Description.Name.c_str());
		try
		{
			Pass.Description.Execute(Context);
		}
		catch (...)
		{
			glPopDebugGroup();
			throw;
		}
		glPopDebugGroup();
		if (Pass.Description.Queue == PassQueue::Graphics)
			InvalidateDiscardedAttachments(Pass, false);
		this->Device->CheckErrors("Render graph pass '" + Pass.Description.Name + "'");
	}
}
void RenderGraph::ReleasePassFramebuffers() noexcept
{
	for (PassResource &Pass : Passes)
		if (Pass.Framebuffer != 0)
			glDeleteFramebuffers(1, &Pass.Framebuffer);
}
void RenderGraph::Reset()
{
	ReleasePassFramebuffers();
	Textures.clear();
	Buffers.clear();
	Passes.clear();
	Compiled = false;
}
bool RenderGraph::IsCompiled() const noexcept
{
	return Compiled;
}
const RenderGraph::TextureResource &RenderGraph::GetTextureResource(TextureHandle Handle) const
{
	if (!Handle.IsValid() || Handle.Value >= Textures.size())
		throw std::out_of_range("Invalid render graph texture handle");
	return Textures[Handle.Value];
}
const RenderGraph::BufferResource &RenderGraph::GetBufferResource(BufferHandle Handle) const
{
	if (!Handle.IsValid() || Handle.Value >= Buffers.size())
		throw std::out_of_range("Invalid render graph buffer handle");
	return Buffers[Handle.Value];
}
const RenderGraph::PassResource &RenderGraph::GetPassResource(PassHandle Handle) const
{
	if (!Handle.IsValid() || Handle.Value >= Passes.size())
		throw std::out_of_range("Invalid render graph pass handle");
	return Passes[Handle.Value];
}
GLuint RenderGraphContext::GetTexture(TextureHandle Handle) const
{
	const RenderGraph::TextureResource &Resource = Graph.GetTextureResource(Handle);
	return Resource.ImportedID != 0 ? Resource.ImportedID : Graph.TexturePool[Resource.PhysicalIndex].Texture;
}
GLuint RenderGraphContext::GetBuffer(BufferHandle Handle) const
{
	const RenderGraph::BufferResource &Resource = Graph.GetBufferResource(Handle);
	return Resource.ImportedID != 0 ? Resource.ImportedID : Graph.BufferPool[Resource.PhysicalIndex].Buffer;
}
Extent2D RenderGraphContext::GetExtent(TextureHandle Handle) const
{
	return Graph.GetTextureResource(Handle).Description.Extent;
}
void RenderGraphContext::BindPassFramebuffer() const
{
	glBindFramebuffer(GL_FRAMEBUFFER, Graph.GetPassResource(Pass).Framebuffer);
}
void RenderGraphContext::ValidateGraphicsPipelineTargets(const pipeline::shader::GraphicsPipeline &Pipeline) const
{
	const RenderGraph::PassResource &CurrentPass = Graph.GetPassResource(Pass);
	if (CurrentPass.Description.Queue != PassQueue::Graphics)
		throw std::logic_error("Only a graphics render-graph pass can bind a graphics pipeline");
	const pipeline::shader::RenderTargetSignature &Expected = Pipeline.GetDescriptor().State.RenderTargets;
	if (Expected.ColorAttachmentCount != CurrentPass.Description.ColorAttachments.size() ||
		Expected.HasDepth != CurrentPass.Description.DepthAttachment.has_value())
		throw std::logic_error("Graphics pipeline render-target signature does not match render-graph pass '" +
							   CurrentPass.Description.Name + "'");
	uint32 ActualSampleCount = 1;
	bool SampleCountInitialized = false;
	auto RequireSampleCount = [this, &ActualSampleCount, &SampleCountInitialized](TextureHandle Handle)
	{
		const uint32 SampleCount = Graph.GetTextureResource(Handle).Description.SampleCount;
		if (!SampleCountInitialized)
		{
			ActualSampleCount = SampleCount;
			SampleCountInitialized = true;
		}
		else if (ActualSampleCount != SampleCount)
			throw std::logic_error("Render graph pass attachments use incompatible sample counts");
	};
	for (const TextureAttachment &Attachment : CurrentPass.Description.ColorAttachments)
		RequireSampleCount(Attachment.Texture);
	if (CurrentPass.Description.DepthAttachment)
		RequireSampleCount(CurrentPass.Description.DepthAttachment->Texture);
	if (SampleCountInitialized && Expected.SampleCount != ActualSampleCount)
		throw std::logic_error("Graphics pipeline sample count does not match render-graph attachments for pass '" +
							   CurrentPass.Description.Name + "'");
}
} // namespace renderer::graph
