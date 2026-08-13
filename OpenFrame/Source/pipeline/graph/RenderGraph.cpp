#include "Source/pipeline/graph/RenderGraph.h"

#include "RenderPass.h"
#include "Source/pipeline/device/Device.h"
#include "Source/pipeline/shader/GraphicsPipeline.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pipeline::graph
{
struct RenderGraph::TextureResource final
{
	TextureDescription Description;
	GLuint ImportedID = 0;
	uint32 PhysicalIndex = ~uint32{0};
	uint64 AllocationGeneration = 0;
	uint32 FirstUse = ~uint32{0};
	uint32 LastUse = 0;
	bool Exported = false;

	void ResetForReuse() noexcept
	{
		this->Description.DebugName.clear();
		this->ImportedID = 0;
		this->PhysicalIndex = ~uint32{0};
		this->AllocationGeneration = 0;
		this->FirstUse = ~uint32{0};
		this->LastUse = 0;
		this->Exported = false;
	}
};
struct RenderGraph::BufferResource final
{
	BufferDescription Description;
	GLuint ImportedID = 0;
	uint32 PhysicalIndex = ~uint32{0};
	uint32 FirstUse = ~uint32{0};
	uint32 LastUse = 0;

	void ResetForReuse() noexcept
	{
		this->Description.DebugName.clear();
		this->ImportedID = 0;
		this->PhysicalIndex = ~uint32{0};
		this->FirstUse = ~uint32{0};
		this->LastUse = 0;
	}
};
struct RenderGraph::PassResource final
{
	PassResource() : Pass(RenderPassDescription{.Name = "__reusable_pass", .Execute = [](RenderGraphContext &) {}})
	{
	}

	RenderPass Pass;
	GLuint Framebuffer = 0;
	GLuint ValidatedFramebuffer = 0;
	uint64 FramebufferSignature = 0;
	bool FramebufferValidated = false;

	[[nodiscard]] const RenderPassDescription &GetDescription() const noexcept
	{
		return this->Pass.GetDescription();
	}
};
struct RenderGraph::PhysicalTexture final
{
	TextureDescription Description;
	GLuint Texture = 0;
	uint64 AllocationGeneration = 0;
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

RenderGraph::RenderGraph(pipeline::device::Device &Device) : Device(Device)
{
	// Reserve the normal hybrid-frame working set so the first graph compile
	// does not grow graph metadata containers one resource at a time.
	this->Textures.reserve(32);
	this->Buffers.reserve(16);
	this->Passes.reserve(24);
	this->TexturePool.reserve(32);
	this->BufferPool.reserve(16);
	this->FramebufferPool.reserve(24);
	this->TextureWrittenScratch.reserve(128);
	this->BufferWrittenScratch.reserve(64);
	this->DrawBuffersScratch.reserve(std::max<uint32>(16U, Device.GetCapabilities().MaximumDrawBuffers));
}

namespace
{
[[nodiscard]] GLenum ToGlInternalFormat(TextureFormat Format)
{
	switch (Format)
	{
	case TextureFormat::Depth32Float:
		return GL_DEPTH_COMPONENT32F;
	case TextureFormat::RGBA8SRGB:
		return GL_SRGB8_ALPHA8;
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
	case TextureFormat::RGBA8SRGB:
		return pipeline::device::DeviceFormat::RGBA8SRGB;
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
		   Left.SampleCount == Right.SampleCount && Left.Sampling == Right.Sampling;
}
[[nodiscard]] bool CanReusePersistentTexture(const TextureDescription &Candidate, const TextureDescription &Requested)
{
	return Candidate.Persistent && Requested.Persistent && Candidate.DebugName == Requested.DebugName &&
		   Candidate.Extent.Width == Requested.Extent.Width && Candidate.Extent.Height == Requested.Extent.Height &&
		   Candidate.Format == Requested.Format && Candidate.Dimension == Requested.Dimension && Candidate.MipCount == Requested.MipCount &&
		   Candidate.Layers >= Requested.Layers && Candidate.SampleCount == Requested.SampleCount &&
		   Candidate.Sampling == Requested.Sampling;
}

template <typename PassResourceType> void InvalidateDiscardedAttachments(const PassResourceType &Pass, bool BeforeExecution)
{
	if (Pass.Framebuffer == 0)
		return;
	for (uint32 ColorIndex = 0; ColorIndex < Pass.GetDescription().ColorAttachments.size(); ++ColorIndex)
	{
		const TextureAttachment &Attachment = Pass.GetDescription().ColorAttachments[ColorIndex];
		const bool Discard = BeforeExecution ? Attachment.Load == LoadOperation::Discard : Attachment.Store == StoreOperation::Discard;
		if (Discard)
		{
			const GLenum AttachmentPoint = GL_COLOR_ATTACHMENT0 + ColorIndex;
			glInvalidateNamedFramebufferData(Pass.Framebuffer, 1, &AttachmentPoint);
		}
	}
	if (Pass.GetDescription().DepthAttachment)
	{
		const DepthAttachment &Attachment = *Pass.GetDescription().DepthAttachment;
		const bool Discard = BeforeExecution ? Attachment.Load == LoadOperation::Discard : Attachment.Store == StoreOperation::Discard;
		if (Discard)
		{
			constexpr GLenum DepthAttachmentPoint = GL_DEPTH_ATTACHMENT;
			glInvalidateNamedFramebufferData(Pass.Framebuffer, 1, &DepthAttachmentPoint);
		}
	}
}
} // namespace

RenderGraph::~RenderGraph()
{
	if (this->Device)
	{
		this->ReleasePassFramebuffers();
		for (PhysicalTexture &Texture : TexturePool)
			if (Texture.Texture != 0)
				this->Device->RetireGPUObject(pipeline::device::GPUObjectType::Texture, Texture.Texture);
		for (PhysicalBuffer &Buffer : BufferPool)
			if (Buffer.Buffer != 0)
				this->Device->RetireGPUObject(pipeline::device::GPUObjectType::Buffer, Buffer.Buffer);
	}
}
void RenderGraph::BeginFrame(const Extent2D Extent, const uint64 ViewIdentity, const uint64 ViewGeneration)
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
						  this->Device->RetireGPUObject(pipeline::device::GPUObjectType::Texture, Texture.Texture);
					  return true;
				  });
	std::erase_if(this->BufferPool,
				  [this](PhysicalBuffer &Buffer)
				  {
					  if (this->FrameSerial - Buffer.LastFrameUsed <= RetainedFrameCount)
						  return false;
					  if (Buffer.Buffer != 0)
						  this->Device->RetireGPUObject(pipeline::device::GPUObjectType::Buffer, Buffer.Buffer);
					  return true;
				  });
	this->FrameExtent = Extent;
	this->ViewIdentity = ViewIdentity;
	this->ViewGeneration = ViewGeneration;
	for (PhysicalTexture &Texture : TexturePool)
		Texture.LastUse = ~uint32{0};
	for (PhysicalBuffer &Buffer : BufferPool)
		Buffer.LastUse = ~uint32{0};
}
TextureHandle RenderGraph::CreateTextureInternal(const string_view DebugName, Extent2D Extent, const TextureFormat Format,
												 const TextureDimension Dimension, const uint32 MipCount, const uint32 Layers,
												 const uint32 SampleCount, const bool Persistent, const TextureSamplingMode Sampling)
{
	if (Compiled)
		throw std::logic_error("Cannot create render graph resources after compile");
	if (!Extent.IsValid())
		Extent = FrameExtent;
	const bool Multisampled = IsMultisampled(Dimension);
	if (!Extent.IsValid() || MipCount == 0 || Layers == 0 || SampleCount == 0 || (Multisampled && (SampleCount == 1 || MipCount != 1)) ||
		(!Multisampled && SampleCount != 1))
		throw std::invalid_argument("Invalid render graph texture description");
	if ((Dimension == TextureDimension::Texture2D || Dimension == TextureDimension::Texture2DMultisample) && Layers != 1)
		throw std::invalid_argument("Non-array render graph textures require exactly one layer");
	if (Sampling == TextureSamplingMode::DepthComparison && (Format != TextureFormat::Depth32Float || Multisampled))
		throw std::invalid_argument("Depth-comparison sampling requires a non-multisampled depth texture");
	if (Dimension == TextureDimension::TextureCubeArray && Layers % 6 != 0)
		throw std::invalid_argument("Cube-array render graph texture layers must be a multiple of six");
	const pipeline::device::DeviceCapabilities &Limits = this->Device->GetCapabilities();
	const uint32 MaximumDimension =
		Dimension == TextureDimension::TextureCubeArray ? Limits.MaximumCubeMapTextureSize : Limits.MaximumTextureSize;
	if (Extent.Width > MaximumDimension || Extent.Height > MaximumDimension)
		throw std::invalid_argument("Render graph texture extent exceeds Device limits");
	if ((Dimension == TextureDimension::Texture2DArray || Dimension == TextureDimension::Texture2DMultisampleArray ||
		 Dimension == TextureDimension::TextureCubeArray) &&
		Layers > Limits.MaximumTextureArrayLayers)
		throw std::invalid_argument("Render graph texture layer count exceeds Device limits");
	const uint32 MaximumMipCount = static_cast<uint32>(std::bit_width(std::max(Extent.Width, Extent.Height)));
	if (MipCount > MaximumMipCount)
		throw std::invalid_argument("Render graph texture mip count exceeds its complete mip chain");
	const pipeline::device::DeviceFormatCapabilities &FormatCapabilities = this->Device->GetFormatCapabilities(ToDeviceFormat(Format));
	if (!FormatCapabilities.Supported)
		throw std::invalid_argument("Render graph texture format is unsupported by the Device");
	if (Multisampled && std::find(FormatCapabilities.SampleCounts.begin(), FormatCapabilities.SampleCounts.end(), SampleCount) ==
							FormatCapabilities.SampleCounts.end())
		throw std::invalid_argument("Render graph texture sample count is unsupported for its format");
	TextureResource *Resource = nullptr;
	if (this->TextureCount < this->Textures.size())
	{
		Resource = &this->Textures[this->TextureCount];
		Resource->ResetForReuse();
	}
	else
	{
		this->Textures.emplace_back();
		Resource = &this->Textures.back();
	}
	Resource->Description.DebugName.assign(DebugName);
	Resource->Description.Extent = Extent;
	Resource->Description.Format = Format;
	Resource->Description.Dimension = Dimension;
	Resource->Description.MipCount = MipCount;
	Resource->Description.Layers = Layers;
	Resource->Description.SampleCount = SampleCount;
	Resource->Description.Persistent = Persistent;
	Resource->Description.Sampling = Sampling;
	return {static_cast<uint32>(this->TextureCount++)};
}
TextureHandle RenderGraph::CreateTexture(TextureDescription Description)
{
	return this->CreateTextureInternal(Description.DebugName, Description.Extent, Description.Format, Description.Dimension,
									   Description.MipCount, Description.Layers, Description.SampleCount, Description.Persistent,
									   Description.Sampling);
}
TextureHandle RenderGraph::CreateTexture(const string_view DebugName, const Extent2D Extent, const TextureFormat Format,
										 const TextureDimension Dimension, const uint32 MipCount, const uint32 Layers,
										 const uint32 SampleCount, const bool Persistent, const TextureSamplingMode Sampling)
{
	return this->CreateTextureInternal(DebugName, Extent, Format, Dimension, MipCount, Layers, SampleCount, Persistent, Sampling);
}
TextureHandle RenderGraph::ImportTexture(TextureDescription Description, GLuint Texture)
{
	if (Texture == 0)
		throw std::invalid_argument("Cannot import OpenGL texture 0");
	TextureHandle Handle = CreateTexture(std::move(Description));
	Textures[Handle.Value].ImportedID = Texture;
	Textures[Handle.Value].AllocationGeneration = this->FrameSerial;
	return Handle;
}
TextureHandle RenderGraph::ImportTexture(const string_view DebugName, const Extent2D Extent, const TextureFormat Format,
										 const GLuint Texture, const TextureDimension Dimension, const uint32 MipCount, const uint32 Layers,
										 const uint32 SampleCount, const bool Persistent)
{
	if (Texture == 0)
		throw std::invalid_argument("Cannot import OpenGL texture 0");
	TextureHandle Handle = this->CreateTextureInternal(DebugName, Extent, Format, Dimension, MipCount, Layers, SampleCount, Persistent,
													   TextureSamplingMode::Regular);
	this->Textures[Handle.Value].ImportedID = Texture;
	this->Textures[Handle.Value].AllocationGeneration = this->FrameSerial;
	return Handle;
}
BufferHandle RenderGraph::CreateBufferInternal(const string_view DebugName, const uint64 SizeInBytes, const GLbitfield StorageFlags,
											   const bool Persistent)
{
	if (Compiled || SizeInBytes == 0)
		throw std::invalid_argument("Invalid render graph buffer description");
	if (SizeInBytes > static_cast<uint64>(std::numeric_limits<GLsizeiptr>::max()))
		throw std::overflow_error("Render graph buffer size exceeds OpenGL limits");
	BufferResource *Resource = nullptr;
	if (this->BufferCount < this->Buffers.size())
	{
		Resource = &this->Buffers[this->BufferCount];
		Resource->ResetForReuse();
	}
	else
	{
		this->Buffers.emplace_back();
		Resource = &this->Buffers.back();
	}
	Resource->Description.DebugName.assign(DebugName);
	Resource->Description.SizeInBytes = SizeInBytes;
	Resource->Description.StorageFlags = StorageFlags;
	Resource->Description.Persistent = Persistent;
	return {static_cast<uint32>(this->BufferCount++)};
}
BufferHandle RenderGraph::CreateBuffer(BufferDescription Description)
{
	return this->CreateBufferInternal(Description.DebugName, Description.SizeInBytes, Description.StorageFlags, Description.Persistent);
}
BufferHandle RenderGraph::CreateBuffer(const string_view DebugName, const uint64 SizeInBytes, const GLbitfield StorageFlags,
									   const bool Persistent)
{
	return this->CreateBufferInternal(DebugName, SizeInBytes, StorageFlags, Persistent);
}
BufferHandle RenderGraph::ImportBuffer(BufferDescription Description, GLuint Buffer)
{
	if (Buffer == 0)
		throw std::invalid_argument("Cannot import OpenGL buffer 0");
	BufferHandle Handle = CreateBuffer(std::move(Description));
	Buffers[Handle.Value].ImportedID = Buffer;
	return Handle;
}
BufferHandle RenderGraph::ImportBuffer(const string_view DebugName, const uint64 SizeInBytes, const GLbitfield StorageFlags,
									   const GLuint Buffer, const bool Persistent)
{
	if (Buffer == 0)
		throw std::invalid_argument("Cannot import OpenGL buffer 0");
	BufferHandle Handle = this->CreateBufferInternal(DebugName, SizeInBytes, StorageFlags, Persistent);
	this->Buffers[Handle.Value].ImportedID = Buffer;
	return Handle;
}
PassHandle RenderGraph::AddPass(RenderPass Pass)
{
	if (Compiled)
		throw std::logic_error("Cannot add a render pass after compile");
	if (this->PassCount < this->Passes.size())
	{
		PassResource &Resource = this->Passes[this->PassCount];
		Resource.Pass = std::move(Pass);
		Resource.Framebuffer = 0;
	}
	else
	{
		this->Passes.emplace_back();
		this->Passes.back().Pass = std::move(Pass);
	}
	return {static_cast<uint32>(this->PassCount++)};
}

PassHandle RenderGraph::AddPass(RenderPassDescription Description)
{
	return this->AddPass(RenderPass(std::move(Description)));
}

PassHandle RenderGraph::AddPass(const string_view Name, const PassQueue Queue, const std::initializer_list<TextureHandle> ReadTextures,
								const std::initializer_list<BufferAccess> ReadBuffers,
								const std::initializer_list<TextureAttachment> ColorAttachments, const std::optional<DepthAttachment> Depth,
								const std::initializer_list<TextureHandle> WriteTextures,
								const std::initializer_list<BufferAccess> WriteBuffers, std::function<void(RenderGraphContext &)> Execute)
{
	if (this->Compiled)
		throw std::logic_error("Cannot add a render pass after compile");
	if (Name.empty() || !Execute)
		throw std::invalid_argument("Render pass requires a name and execute callback");
	PassResource *Resource = nullptr;
	if (this->PassCount < this->Passes.size())
	{
		Resource = &this->Passes[this->PassCount];
		Resource->Pass.ResetForReuse();
	}
	else
	{
		this->Passes.emplace_back();
		Resource = &this->Passes.back();
	}
	Resource->Framebuffer = 0;
	RenderPassDescription &Description = Resource->Pass.GetMutableDescription();
	Description.Name.assign(Name);
	Description.Queue = Queue;
	Description.ReadTextures.assign(ReadTextures.begin(), ReadTextures.end());
	Description.ReadBuffers.assign(ReadBuffers.begin(), ReadBuffers.end());
	Description.ColorAttachments.assign(ColorAttachments.begin(), ColorAttachments.end());
	Description.DepthAttachment = Depth;
	Description.WriteTextures.assign(WriteTextures.begin(), WriteTextures.end());
	Description.WriteBuffers.assign(WriteBuffers.begin(), WriteBuffers.end());
	Description.Execute = std::move(Execute);
	return {static_cast<uint32>(this->PassCount++)};
}

void RenderGraph::ExportTexture(const TextureHandle Handle)
{
	if (this->Compiled)
		throw std::logic_error("Cannot export a render graph texture after compile");
	if (!Handle.IsValid() || Handle.Value >= this->TextureCount)
		throw std::out_of_range("Invalid render graph texture handle");
	TextureResource &Resource = this->Textures[Handle.Value];
	if (Resource.Description.Dimension != TextureDimension::Texture2D)
		throw std::invalid_argument("Only single-sample two-dimensional textures can be exported for external consumption");
	if (Resource.Description.SampleCount != 1 || Resource.Description.Layers != 1)
		throw std::invalid_argument("Exported render graph textures cannot be multisampled or layered");
	Resource.Exported = true;
}

ExportedTexture RenderGraph::GetExportedTexture(const TextureHandle Handle) const
{
	if (!this->Compiled)
		throw std::logic_error("Render graph must be compiled before resolving an exported texture");
	const TextureResource &Resource = this->GetTextureResource(Handle);
	if (!Resource.Exported)
		throw std::logic_error("Render graph texture was not declared as an export");
	const GLuint Texture = Resource.ImportedID != 0 ? Resource.ImportedID : this->TexturePool.at(Resource.PhysicalIndex).Texture;
	return {.Texture = Texture,
			.Extent = Resource.Description.Extent,
			.Format = Resource.Description.Format,
			.Dimension = Resource.Description.Dimension,
			.MipCount = Resource.Description.MipCount,
			.Layers = Resource.Description.Layers,
			.SampleCount = Resource.Description.SampleCount,
			.FrameSerial = this->FrameSerial,
			.ViewIdentity = this->ViewIdentity,
			.ViewGeneration = this->ViewGeneration};
}

void RenderGraph::Validate()
{
	this->TextureWrittenScratch.assign(this->TextureCount, false);
	this->BufferWrittenScratch.assign(this->BufferCount, false);
	std::vector<bool> &TextureWritten = this->TextureWrittenScratch;
	std::vector<bool> &BufferWritten = this->BufferWrittenScratch;
	auto FindFutureTextureProducer = [this](TextureHandle Handle, uint32 AfterPass) -> const std::string *
	{
		for (uint32 PassIndex = AfterPass + 1U; PassIndex < this->PassCount; ++PassIndex)
		{
			const RenderPassDescription &Candidate = Passes[PassIndex].GetDescription();
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
		for (uint32 PassIndex = AfterPass + 1U; PassIndex < this->PassCount; ++PassIndex)
		{
			const RenderPassDescription &Candidate = Passes[PassIndex].GetDescription();
			if (std::any_of(Candidate.WriteBuffers.begin(), Candidate.WriteBuffers.end(),
							[Handle](const BufferAccess &Written) { return Written.Buffer.Value == Handle.Value; }))
				return &Candidate.Name;
		}
		return nullptr;
	};
	for (uint32 Index = 0; Index < this->PassCount; ++Index)
	{
		const RenderPassDescription &Pass = Passes[Index].GetDescription();
		if (Pass.ColorAttachments.size() > this->Device->GetCapabilities().MaximumColorAttachments ||
			Pass.ColorAttachments.size() > this->Device->GetCapabilities().MaximumDrawBuffers)
			throw std::logic_error("Render pass declares more color attachments than the Device supports: " + Pass.Name);

		const TextureDescription *AttachmentLayout = nullptr;
		auto ValidateAttachmentLayout = [&](const TextureHandle Handle, const bool Depth)
		{
			if (!Handle.IsValid() || Handle.Value >= this->TextureCount)
				throw std::logic_error("Render pass references an invalid framebuffer attachment");
			const TextureDescription &Description = Textures[Handle.Value].Description;
			if (AttachmentLayout == nullptr)
			{
				AttachmentLayout = &Description;
			}
			else if (Description.Extent.Width != AttachmentLayout->Extent.Width ||
					 Description.Extent.Height != AttachmentLayout->Extent.Height || Description.Dimension != AttachmentLayout->Dimension ||
					 Description.Layers != AttachmentLayout->Layers || Description.SampleCount != AttachmentLayout->SampleCount)
			{
				throw std::logic_error("Render pass framebuffer attachments have incompatible dimensions, layers, or sample counts: " +
									   Pass.Name);
			}
			const pipeline::device::DeviceFormatCapabilities &Format =
				this->Device->GetFormatCapabilities(ToDeviceFormat(Description.Format));
			if (Depth ? !Format.DepthRenderable : !Format.ColorRenderable)
				throw std::logic_error("Render pass framebuffer attachment has an incompatible format role: " + Pass.Name);
		};
		for (uint32 ColorIndex = 0; ColorIndex < Pass.ColorAttachments.size(); ++ColorIndex)
		{
			const TextureAttachment &Attachment = Pass.ColorAttachments[ColorIndex];
			if (std::any_of(Pass.ColorAttachments.begin(), Pass.ColorAttachments.begin() + ColorIndex,
							[&Attachment](const TextureAttachment &Previous)
							{ return Previous.Texture.Value == Attachment.Texture.Value; }))
				throw std::logic_error("Render pass attaches the same texture to multiple color slots: " + Pass.Name);
			ValidateAttachmentLayout(Attachment.Texture, false);
		}
		if (Pass.DepthAttachment)
		{
			if (std::any_of(Pass.ColorAttachments.begin(), Pass.ColorAttachments.end(), [&](const TextureAttachment &Attachment)
							{ return Attachment.Texture.Value == Pass.DepthAttachment->Texture.Value; }))
				throw std::logic_error("Render pass attaches one texture as both color and depth: " + Pass.Name);
			ValidateAttachmentLayout(Pass.DepthAttachment->Texture, true);
		}

		auto RequireTexture = [&](TextureHandle Handle, bool RequireWrite)
		{
			if (!Handle.IsValid() || Handle.Value >= this->TextureCount)
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
			if (!Handle.IsValid() || Handle.Value >= this->BufferCount)
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
		for (const BufferAccess &Access : Pass.ReadBuffers)
			RequireBuffer(Access.Buffer, true);
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
		for (const BufferAccess &Access : Pass.WriteBuffers)
		{
			RequireBuffer(Access.Buffer, false);
			BufferWritten[Access.Buffer.Value] = true;
		}
	}
	for (uint32 Index = 0; Index < this->TextureCount; ++Index)
	{
		const TextureResource &Resource = this->Textures[Index];
		if (Resource.Exported && Resource.ImportedID == 0 && !TextureWritten[Index])
			throw std::logic_error("Render graph exports texture before it is written: " + Resource.Description.DebugName);
	}
}

void RenderGraph::AllocateResources()
{
	for (uint32 PassIndex = 0; PassIndex < this->PassCount; ++PassIndex)
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
		const RenderPassDescription &Pass = Passes[PassIndex].GetDescription();
		for (TextureHandle Handle : Pass.ReadTextures)
			MarkTexture(Handle);
		for (const TextureAttachment &Attachment : Pass.ColorAttachments)
			MarkTexture(Attachment.Texture);
		if (Pass.DepthAttachment)
			MarkTexture(Pass.DepthAttachment->Texture);
		for (TextureHandle Handle : Pass.WriteTextures)
			MarkTexture(Handle);
		for (const BufferAccess &Access : Pass.ReadBuffers)
			MarkBuffer(Access.Buffer);
		for (const BufferAccess &Access : Pass.WriteBuffers)
			MarkBuffer(Access.Buffer);
	}
	for (usize Index = 0; Index < this->TextureCount; ++Index)
	{
		TextureResource &Resource = this->Textures[Index];
		if (Resource.Exported)
			Resource.LastUse = static_cast<uint32>(this->PassCount);
		if (Resource.ImportedID != 0)
			continue;
		auto Physical = std::find_if(TexturePool.begin(), TexturePool.end(),
									 [&](const PhysicalTexture &Candidate)
									 {
										 if (Resource.Description.Persistent)
											 return CanReusePersistentTexture(Candidate.Description, Resource.Description);
										 if (!SameTexture(Candidate.Description, Resource.Description))
											 return false;
										 return !Candidate.Description.Persistent &&
												(Candidate.LastUse == ~uint32{0} || Candidate.LastUse < Resource.FirstUse);
									 });
		if (Physical == TexturePool.end())
		{
			PhysicalTexture Created{.Description = Resource.Description};
			Created.AllocationGeneration = this->NextTextureAllocationGeneration++;
			if (this->NextTextureAllocationGeneration == 0)
				this->NextTextureAllocationGeneration = 1;
			glCreateTextures(ToGlTarget(Created.Description.Dimension), 1, &Created.Texture);
			if (Created.Texture == 0)
				throw std::runtime_error("Render graph could not allocate texture " + Created.Description.DebugName);
			try
			{
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
						Created.Texture, static_cast<GLsizei>(Created.Description.SampleCount),
						ToGlInternalFormat(Created.Description.Format), static_cast<GLsizei>(Created.Description.Extent.Width),
						static_cast<GLsizei>(Created.Description.Extent.Height), static_cast<GLsizei>(Created.Description.Layers), GL_TRUE);
				if (!IsMultisampled(Created.Description.Dimension))
				{
					const bool Filterable = Created.Description.Sampling == TextureSamplingMode::DepthComparison ||
											this->Device->GetFormatCapabilities(ToDeviceFormat(Created.Description.Format)).Filterable;
					glTextureParameteri(Created.Texture, GL_TEXTURE_MIN_FILTER,
										Filterable ? (Created.Description.MipCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR) : GL_NEAREST);
					glTextureParameteri(Created.Texture, GL_TEXTURE_MAG_FILTER, Filterable ? GL_LINEAR : GL_NEAREST);
					glTextureParameteri(Created.Texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTextureParameteri(Created.Texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					if (Created.Description.Dimension == TextureDimension::Texture2DArray ||
						Created.Description.Dimension == TextureDimension::TextureCubeArray)
						glTextureParameteri(Created.Texture, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
					if (Created.Description.Sampling == TextureSamplingMode::DepthComparison)
					{
						glTextureParameteri(Created.Texture, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
						glTextureParameteri(Created.Texture, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
					}
				}
				glObjectLabel(GL_TEXTURE, Created.Texture, static_cast<GLsizei>(Created.Description.DebugName.size()),
							  Created.Description.DebugName.c_str());
				this->Device->CheckErrors("Render graph texture allocation " + Created.Description.DebugName);
				TexturePool.push_back(Created);
			}
			catch (...)
			{
				glDeleteTextures(1, &Created.Texture);
				throw;
			}
			Resource.PhysicalIndex = static_cast<uint32>(TexturePool.size() - 1);
		}
		else
		{
			Resource.PhysicalIndex = static_cast<uint32>(Physical - TexturePool.begin());
		}
		Resource.AllocationGeneration = TexturePool[Resource.PhysicalIndex].AllocationGeneration;
		TexturePool[Resource.PhysicalIndex].LastUse = Resource.LastUse;
		TexturePool[Resource.PhysicalIndex].LastFrameUsed = this->FrameSerial;
	}
	for (usize Index = 0; Index < this->BufferCount; ++Index)
	{
		BufferResource &Resource = this->Buffers[Index];
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
			if (Created.Buffer == 0)
				throw std::runtime_error("Render graph could not allocate buffer " + Created.Description.DebugName);
			try
			{
				glNamedBufferStorage(Created.Buffer, static_cast<GLsizeiptr>(Created.Description.SizeInBytes), nullptr,
									 Created.Description.StorageFlags);
				glObjectLabel(GL_BUFFER, Created.Buffer, static_cast<GLsizei>(Created.Description.DebugName.size()),
							  Created.Description.DebugName.c_str());
				this->Device->CheckErrors("Render graph buffer allocation " + Created.Description.DebugName);
				BufferPool.push_back(Created);
			}
			catch (...)
			{
				glDeleteBuffers(1, &Created.Buffer);
				throw;
			}
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
	const auto Mix = [](uint64 &Hash, const uint64 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ULL;
	};
	const auto TextureSignature = [this, &Mix](const TextureHandle Handle)
	{
		const TextureResource &Resource = this->GetTextureResource(Handle);
		const GLuint Texture = Resource.ImportedID != 0 ? Resource.ImportedID : TexturePool[Resource.PhysicalIndex].Texture;
		uint64 Hash = 1469598103934665603ULL;
		Mix(Hash, Texture);
		Mix(Hash, Resource.AllocationGeneration);
		Mix(Hash, static_cast<uint64>(Resource.Description.Format));
		Mix(Hash, static_cast<uint64>(Resource.Description.Dimension));
		Mix(Hash, Resource.Description.Extent.Width);
		Mix(Hash, Resource.Description.Extent.Height);
		Mix(Hash, Resource.Description.MipCount);
		Mix(Hash, Resource.Description.Layers);
		Mix(Hash, Resource.Description.SampleCount);
		return Hash;
	};
	for (uint32 PassIndex = 0; PassIndex < this->PassCount; ++PassIndex)
	{
		PassResource &Pass = this->Passes[PassIndex];
		if (Pass.GetDescription().Queue != PassQueue::Graphics ||
			(Pass.GetDescription().ColorAttachments.empty() && !Pass.GetDescription().DepthAttachment))
			continue;
		if (this->FramebufferCursor == this->FramebufferPool.size())
		{
			GLuint Framebuffer = 0;
			glCreateFramebuffers(1, &Framebuffer);
			if (Framebuffer == 0)
				throw std::runtime_error("Render graph failed to create a framebuffer");
			try
			{
				this->FramebufferPool.push_back(Framebuffer);
			}
			catch (...)
			{
				glDeleteFramebuffers(1, &Framebuffer);
				throw;
			}
		}
		Pass.Framebuffer = this->FramebufferPool[this->FramebufferCursor++];
		glObjectLabel(GL_FRAMEBUFFER, Pass.Framebuffer, static_cast<GLsizei>(Pass.GetDescription().Name.size()),
					  Pass.GetDescription().Name.c_str());
		for (uint32 ColorIndex = 0; ColorIndex < this->Device->GetCapabilities().MaximumColorAttachments; ++ColorIndex)
			glNamedFramebufferTexture(Pass.Framebuffer, GL_COLOR_ATTACHMENT0 + ColorIndex, 0, 0);
		glNamedFramebufferTexture(Pass.Framebuffer, GL_DEPTH_ATTACHMENT, 0, 0);
		const uint32 MaximumDrawBuffers = this->Device->GetCapabilities().MaximumDrawBuffers;
		this->DrawBuffersScratch.assign(MaximumDrawBuffers, GL_NONE);
		for (uint32 ColorIndex = 0; ColorIndex < Pass.GetDescription().ColorAttachments.size(); ++ColorIndex)
		{
			glNamedFramebufferTexture(
				Pass.Framebuffer, GL_COLOR_ATTACHMENT0 + ColorIndex,
				GetTextureResource(Pass.GetDescription().ColorAttachments[ColorIndex].Texture).ImportedID != 0
					? GetTextureResource(Pass.GetDescription().ColorAttachments[ColorIndex].Texture).ImportedID
					: TexturePool[GetTextureResource(Pass.GetDescription().ColorAttachments[ColorIndex].Texture).PhysicalIndex].Texture,
				0);
			this->DrawBuffersScratch[ColorIndex] = GL_COLOR_ATTACHMENT0 + ColorIndex;
		}
		if (Pass.GetDescription().DepthAttachment)
		{
			const TextureResource &Depth = GetTextureResource(Pass.GetDescription().DepthAttachment->Texture);
			glNamedFramebufferTexture(Pass.Framebuffer, GL_DEPTH_ATTACHMENT,
									  Depth.ImportedID != 0 ? Depth.ImportedID : TexturePool[Depth.PhysicalIndex].Texture, 0);
		}
		glNamedFramebufferDrawBuffers(Pass.Framebuffer, static_cast<GLsizei>(this->DrawBuffersScratch.size()),
									  this->DrawBuffersScratch.data());
		glNamedFramebufferReadBuffer(Pass.Framebuffer, Pass.GetDescription().ColorAttachments.empty() ? GL_NONE : GL_COLOR_ATTACHMENT0);
		uint64 Signature = 1469598103934665603ULL;
		Mix(Signature, Pass.Framebuffer);
		Mix(Signature, static_cast<uint64>(Pass.GetDescription().ColorAttachments.size()));
		for (const TextureAttachment &Attachment : Pass.GetDescription().ColorAttachments)
			Mix(Signature, TextureSignature(Attachment.Texture));
		Mix(Signature, Pass.GetDescription().DepthAttachment.has_value() ? 1U : 0U);
		if (Pass.GetDescription().DepthAttachment.has_value())
			Mix(Signature, TextureSignature(Pass.GetDescription().DepthAttachment->Texture));
		// Attachment writes are cheap and keep the graph deterministic, but status
		// validation is a query. Revalidate only when the framebuffer object or
		// any attachment/storage identity changes.
		const bool NeedsValidation =
			!Pass.FramebufferValidated || Pass.ValidatedFramebuffer != Pass.Framebuffer || Pass.FramebufferSignature != Signature;
		if (NeedsValidation)
		{
			if (glCheckNamedFramebufferStatus(Pass.Framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				throw std::runtime_error("Render graph created an incomplete framebuffer for pass " + Pass.GetDescription().Name);
			Pass.ValidatedFramebuffer = Pass.Framebuffer;
			Pass.FramebufferSignature = Signature;
			Pass.FramebufferValidated = true;
		}
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
	const auto WritesTexture = [](const RenderPassDescription &Description, const TextureHandle Handle)
	{
		return std::any_of(Description.WriteTextures.begin(), Description.WriteTextures.end(),
						   [Handle](const TextureHandle Candidate) { return Candidate.Value == Handle.Value; }) ||
			   std::any_of(Description.ColorAttachments.begin(), Description.ColorAttachments.end(),
						   [Handle](const TextureAttachment &Attachment) { return Attachment.Texture.Value == Handle.Value; }) ||
			   (Description.DepthAttachment.has_value() && Description.DepthAttachment->Texture.Value == Handle.Value);
	};
	const auto WritesBuffer = [](const RenderPassDescription &Description, const BufferHandle Handle)
	{
		return std::any_of(Description.WriteBuffers.begin(), Description.WriteBuffers.end(),
						   [Handle](const BufferAccess &Candidate) { return Candidate.Buffer.Value == Handle.Value; });
	};
	const auto BarrierForBufferUsage = [](const BufferUsage Usage)
	{
		const uint16 Value = static_cast<uint16>(Usage);
		const auto Has = [Value](const BufferUsage Candidate) { return (Value & static_cast<uint16>(Candidate)) != 0; };
		GLbitfield Barrier = 0;
		if (Has(BufferUsage::ShaderStorage))
			Barrier |= GL_SHADER_STORAGE_BARRIER_BIT;
		if (Has(BufferUsage::Uniform))
			Barrier |= GL_UNIFORM_BARRIER_BIT;
		if (Has(BufferUsage::Vertex))
			Barrier |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
		if (Has(BufferUsage::Index))
			Barrier |= GL_ELEMENT_ARRAY_BARRIER_BIT;
		if (Has(BufferUsage::Indirect))
			Barrier |= GL_COMMAND_BARRIER_BIT;
		if (Has(BufferUsage::AtomicCounter))
			Barrier |= GL_ATOMIC_COUNTER_BARRIER_BIT;
		if (Has(BufferUsage::TextureBuffer))
			Barrier |= GL_TEXTURE_FETCH_BARRIER_BIT;
		if (Has(BufferUsage::PixelBuffer))
			Barrier |= GL_PIXEL_BUFFER_BARRIER_BIT;
		if (Has(BufferUsage::Transfer))
			Barrier |= GL_BUFFER_UPDATE_BARRIER_BIT;
		return Barrier;
	};
	for (uint32 Index = 0; Index < this->PassCount; ++Index)
	{
		GLbitfield Barriers = 0;
		const RenderPassDescription &Current = Passes[Index].GetDescription();
		// A producer does not have to be immediately adjacent to its consumer.
		// Derive barriers from the declared resource overlap so an unrelated
		// intervening pass cannot hide a required visibility transition.
		for (uint32 PreviousIndex = 0; PreviousIndex < Index; ++PreviousIndex)
		{
			const RenderPassDescription &Previous = Passes[PreviousIndex].GetDescription();
			for (const TextureHandle Handle : Current.ReadTextures)
			{
				if (WritesTexture(Previous, Handle))
					Barriers |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT;
			}
			for (const TextureHandle Handle : Current.WriteTextures)
			{
				if (WritesTexture(Previous, Handle))
					Barriers |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT;
			}
			for (const TextureAttachment &Attachment : Current.ColorAttachments)
			{
				if (WritesTexture(Previous, Attachment.Texture))
					Barriers |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT;
			}
			if (Current.DepthAttachment.has_value() && WritesTexture(Previous, Current.DepthAttachment->Texture))
				Barriers |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT;
			for (const BufferAccess &Access : Current.ReadBuffers)
			{
				if (WritesBuffer(Previous, Access.Buffer))
					Barriers |= BarrierForBufferUsage(Access.Usage);
			}
			for (const BufferAccess &Access : Current.WriteBuffers)
			{
				if (WritesBuffer(Previous, Access.Buffer))
					Barriers |= BarrierForBufferUsage(Access.Usage);
			}
		}
		if (Barriers != 0)
			glMemoryBarrier(Barriers);
		PassResource &Pass = Passes[Index];
		RenderGraphContext Context(*this, {Index});
		if (Pass.GetDescription().Queue == PassQueue::Graphics)
		{
			Context.BindPassFramebuffer();
			Extent2D ViewportExtent = FrameExtent;
			if (!Pass.GetDescription().ColorAttachments.empty())
				ViewportExtent = GetTextureResource(Pass.GetDescription().ColorAttachments.front().Texture).Description.Extent;
			else if (Pass.GetDescription().DepthAttachment)
				ViewportExtent = GetTextureResource(Pass.GetDescription().DepthAttachment->Texture).Description.Extent;
			else if (!Pass.GetDescription().ReadTextures.empty())
				ViewportExtent = GetTextureResource(Pass.GetDescription().ReadTextures.front()).Description.Extent;
			glViewport(0, 0, static_cast<GLsizei>(ViewportExtent.Width), static_cast<GLsizei>(ViewportExtent.Height));
			InvalidateDiscardedAttachments(Pass, true);
			const bool ClearsColor = std::ranges::any_of(Pass.GetDescription().ColorAttachments, [](const TextureAttachment &Attachment)
														 { return Attachment.Load == LoadOperation::Clear; });
			const bool ClearsDepth =
				Pass.GetDescription().DepthAttachment.has_value() && Pass.GetDescription().DepthAttachment->Load == LoadOperation::Clear;
			if (ClearsColor || ClearsDepth)
			{
				// OpenGL framebuffer clears obey scissor and write masks. A render-graph
				// load operation must clear the complete attachment independently of
				// state left by the preceding pass or previous frame.
				glDisable(GL_SCISSOR_TEST);
				if (ClearsColor)
					glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				if (ClearsDepth)
					glDepthMask(GL_TRUE);
				this->Device->InvalidateGraphicsPipelineStateCache();
			}
			for (uint32 AttachmentIndex = 0; AttachmentIndex < Pass.GetDescription().ColorAttachments.size(); ++AttachmentIndex)
			{
				const TextureAttachment &Attachment = Pass.GetDescription().ColorAttachments[AttachmentIndex];
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
			if (Pass.GetDescription().DepthAttachment && Pass.GetDescription().DepthAttachment->Load == LoadOperation::Clear)
			{
				float32 ClearDepth = Pass.GetDescription().DepthAttachment->ClearDepth;
				glClearNamedFramebufferfv(Pass.Framebuffer, GL_DEPTH, 0, &ClearDepth);
			}
		}
		glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, Index, static_cast<GLsizei>(Pass.GetDescription().Name.size()),
						 Pass.GetDescription().Name.c_str());
		try
		{
			Pass.GetDescription().Execute(Context);
		}
		catch (...)
		{
			glPopDebugGroup();
			throw;
		}
		glPopDebugGroup();
		if (Pass.GetDescription().Queue == PassQueue::Graphics)
			InvalidateDiscardedAttachments(Pass, false);
		// Avoid synchronous error polling here. Render-graph execution is the frame
		// hot path; creation/compile paths perform immediate checks, while the debug
		// callback and device-reset validation report asynchronous runtime faults.
	}
}
void RenderGraph::ReleasePassFramebuffers() noexcept
{
	for (GLuint &Framebuffer : this->FramebufferPool)
		if (Framebuffer != 0)
			this->Device->RetireGPUObject(pipeline::device::GPUObjectType::Framebuffer, Framebuffer);
	this->FramebufferPool.clear();
	this->FramebufferCursor = 0;
}
void RenderGraph::Reset()
{
	this->FramebufferCursor = 0;
	for (usize Index = 0; Index < this->TextureCount; ++Index)
		this->Textures[Index].ResetForReuse();
	for (usize Index = 0; Index < this->BufferCount; ++Index)
		this->Buffers[Index].ResetForReuse();
	this->TextureCount = 0;
	this->BufferCount = 0;
	for (uint32 Index = 0; Index < this->PassCount; ++Index)
	{
		this->Passes[Index].Pass.ResetForReuse();
		this->Passes[Index].Framebuffer = 0;
	}
	this->PassCount = 0;
	Compiled = false;
}
bool RenderGraph::IsCompiled() const noexcept
{
	return Compiled;
}

void RenderGraph::InspectInto(RenderGraphInspection &Result) const
{
	Result.FrameSerial = this->FrameSerial;
	Result.Passes.resize(this->PassCount);
	for (usize Index = 0; Index < this->PassCount; ++Index)
		Result.Passes[Index] = this->Passes[Index].GetDescription().Name;
	Result.Textures.resize(this->TextureCount);
	for (usize Index = 0; Index < this->TextureCount; ++Index)
		Result.Textures[Index] = this->Textures[Index].Description.DebugName;
	Result.Buffers.resize(this->BufferCount);
	for (usize Index = 0; Index < this->BufferCount; ++Index)
		Result.Buffers[Index] = this->Buffers[Index].Description.DebugName;
}
RenderGraphInspection RenderGraph::Inspect() const
{
	RenderGraphInspection Result;
	this->InspectInto(Result);
	return Result;
}
const RenderGraph::TextureResource &RenderGraph::GetTextureResource(TextureHandle Handle) const
{
	if (!Handle.IsValid() || Handle.Value >= this->TextureCount)
		throw std::out_of_range("Invalid render graph texture handle");
	return Textures[Handle.Value];
}
const RenderGraph::BufferResource &RenderGraph::GetBufferResource(BufferHandle Handle) const
{
	if (!Handle.IsValid() || Handle.Value >= this->BufferCount)
		throw std::out_of_range("Invalid render graph buffer handle");
	return Buffers[Handle.Value];
}
const RenderGraph::PassResource &RenderGraph::GetPassResource(PassHandle Handle) const
{
	if (!Handle.IsValid() || Handle.Value >= this->PassCount)
		throw std::out_of_range("Invalid render graph pass handle");
	return Passes[Handle.Value];
}
GLuint RenderGraphContext::GetTexture(TextureHandle Handle) const
{
	this->RequireTextureAccess(Handle);
	const RenderGraph::TextureResource &Resource = Graph.GetTextureResource(Handle);
	return Resource.ImportedID != 0 ? Resource.ImportedID : Graph.TexturePool[Resource.PhysicalIndex].Texture;
}
GLuint RenderGraphContext::GetBuffer(BufferHandle Handle) const
{
	this->RequireBufferAccess(Handle);
	const RenderGraph::BufferResource &Resource = Graph.GetBufferResource(Handle);
	return Resource.ImportedID != 0 ? Resource.ImportedID : Graph.BufferPool[Resource.PhysicalIndex].Buffer;
}
Extent2D RenderGraphContext::GetExtent(TextureHandle Handle) const
{
	this->RequireTextureAccess(Handle);
	return Graph.GetTextureResource(Handle).Description.Extent;
}
void RenderGraphContext::RequireTextureAccess(const TextureHandle Handle) const
{
	const RenderPassDescription &Description = this->Graph.GetPassResource(this->Pass).GetDescription();
	const bool Declared = std::ranges::any_of(Description.ReadTextures,
											  [Handle](const TextureHandle Candidate) { return Candidate.Value == Handle.Value; }) ||
						  std::ranges::any_of(Description.WriteTextures,
											  [Handle](const TextureHandle Candidate) { return Candidate.Value == Handle.Value; }) ||
						  std::ranges::any_of(Description.ColorAttachments, [Handle](const TextureAttachment &Attachment)
											  { return Attachment.Texture.Value == Handle.Value; }) ||
						  (Description.DepthAttachment.has_value() && Description.DepthAttachment->Texture.Value == Handle.Value);
	if (!Declared)
		throw std::logic_error("Render-graph pass '" + Description.Name + "' accessed an undeclared texture");
}
void RenderGraphContext::RequireBufferAccess(const BufferHandle Handle) const
{
	const RenderPassDescription &Description = this->Graph.GetPassResource(this->Pass).GetDescription();
	const bool Declared = std::ranges::any_of(Description.ReadBuffers,
											  [Handle](const BufferAccess &Candidate) { return Candidate.Buffer.Value == Handle.Value; }) ||
						  std::ranges::any_of(Description.WriteBuffers,
											  [Handle](const BufferAccess &Candidate) { return Candidate.Buffer.Value == Handle.Value; });
	if (!Declared)
		throw std::logic_error("Render-graph pass '" + Description.Name + "' accessed an undeclared buffer");
}
void RenderGraphContext::BindPassFramebuffer() const
{
	glBindFramebuffer(GL_FRAMEBUFFER, Graph.GetPassResource(Pass).Framebuffer);
}
void RenderGraphContext::ValidateGraphicsPipelineTargets(const pipeline::shader::GraphicsPipeline &Pipeline) const
{
	const RenderGraph::PassResource &CurrentPass = Graph.GetPassResource(Pass);
	if (CurrentPass.GetDescription().Queue != PassQueue::Graphics)
		throw std::logic_error("Only a graphics render-graph pass can bind a graphics pipeline");
	const pipeline::shader::RenderTargetSignature &Expected = Pipeline.GetDescriptor().State.RenderTargets;
	if (Expected.ColorAttachmentCount != CurrentPass.GetDescription().ColorAttachments.size() ||
		Expected.HasDepth != CurrentPass.GetDescription().DepthAttachment.has_value())
		throw std::logic_error("Graphics pipeline render-target signature does not match render-graph pass '" +
							   CurrentPass.GetDescription().Name + "'");
	for (usize ColorIndex = 0; ColorIndex < CurrentPass.GetDescription().ColorAttachments.size(); ++ColorIndex)
	{
		const GLenum ExpectedFormat = Expected.ColorFormats[ColorIndex];
		if (ExpectedFormat == GL_NONE)
			continue;
		const TextureDescription &Actual =
			Graph.GetTextureResource(CurrentPass.GetDescription().ColorAttachments[ColorIndex].Texture).Description;
		if (ExpectedFormat != ToGlInternalFormat(Actual.Format))
			throw std::logic_error("Graphics pipeline color format does not match render-graph attachment " + std::to_string(ColorIndex) +
								   " for pass '" + CurrentPass.GetDescription().Name + "'");
	}
	if (CurrentPass.GetDescription().DepthAttachment && Expected.DepthFormat != GL_NONE)
	{
		const TextureDescription &Actual = Graph.GetTextureResource(CurrentPass.GetDescription().DepthAttachment->Texture).Description;
		if (Expected.DepthFormat != ToGlInternalFormat(Actual.Format))
			throw std::logic_error("Graphics pipeline depth format does not match the render-graph attachment for pass '" +
								   CurrentPass.GetDescription().Name + "'");
	}
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
	for (const TextureAttachment &Attachment : CurrentPass.GetDescription().ColorAttachments)
		RequireSampleCount(Attachment.Texture);
	if (CurrentPass.GetDescription().DepthAttachment)
		RequireSampleCount(CurrentPass.GetDescription().DepthAttachment->Texture);
	if (SampleCountInitialized && Expected.SampleCount != ActualSampleCount)
		throw std::logic_error("Graphics pipeline sample count does not match render-graph attachments for pass '" +
							   CurrentPass.GetDescription().Name + "'");
}
} // namespace pipeline::graph
