#include "Texture2D.h"

#include "src/pipeline/device/Device.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace renderer::texture
{
namespace
{
[[nodiscard]] int32 CalculateMipMapLevels(const int32 Width, const int32 Height)
{
	return static_cast<int32>(std::bit_width(static_cast<uint32>(std::max(Width, Height))));
}
} // namespace

const Texture2DSpecification Texture2DSpecification::DefaultInstance{.InternalFormat = GL_RGBA8,
																	 .DataFormat = GL_RGBA,
																	 .DataType = GL_UNSIGNED_BYTE,
																	 .MinFilter = GL_LINEAR_MIPMAP_LINEAR,
																	 .MagFilter = GL_LINEAR,
																	 .WrapS = GL_REPEAT,
																	 .WrapT = GL_REPEAT,
																	 .Anisotropy = 16.0f};

Texture2D::Texture2D(pipeline::device::Device &Device, std::string TextureName, const Texture2DCreationInfo &Info)
	: Device(&Device), Name(std::move(TextureName)), Width(Info.Width), Height(Info.Height), Channels(Info.Channels)
{
	(void)this->Device->RequireCurrentContext();
	if (this->Name.empty())
		throw Texture2DError("Texture2D requires a non-empty debug name");
	if (this->Width <= 0 || this->Height <= 0)
		throw Texture2DError("Texture2D dimensions must be positive");
	if (this->Channels <= 0 || this->Channels > 4)
		throw Texture2DError("Texture2D channel count must be between one and four");
	if (Info.Pixels == nullptr)
		throw Texture2DError("Texture2D requires non-null initial pixels");
	const uint32 MaximumTextureSize = this->Device->GetCapabilities().MaximumTextureSize;
	if (static_cast<uint32>(this->Width) > MaximumTextureSize || static_cast<uint32>(this->Height) > MaximumTextureSize)
	{
		throw Texture2DError("Texture2D dimensions exceed Device limits");
	}
	if (!this->Device->GetCapabilities().BindlessTextures)
	{
		throw Texture2DError("Texture2D requires GL_ARB_bindless_texture for the engine material contract");
	}

	this->Device->CheckErrors("Texture2D construction precondition");
	glCreateTextures(GL_TEXTURE_2D, 1, &this->TextureID);
	try
	{
		this->MipMapLevels = CalculateMipMapLevels(this->Width, this->Height);
		glTextureStorage2D(this->TextureID, this->MipMapLevels, Info.Specification.InternalFormat, this->Width, this->Height);
		glTextureSubImage2D(this->TextureID, 0, 0, 0, this->Width, this->Height, Info.Specification.DataFormat, Info.Specification.DataType,
							Info.Pixels);
		glGenerateTextureMipmap(this->TextureID);
		this->Device->CheckErrors("Texture2D immutable storage and upload");
		if (Info.Specification.InternalFormat == GL_RGBA8)
		{
			glGenTextures(1, &this->SRGBTextureView);
			glTextureView(this->SRGBTextureView, GL_TEXTURE_2D, this->TextureID, GL_SRGB8_ALPHA8, 0, this->MipMapLevels, 0, 1);
		}
		else if (Info.Specification.InternalFormat == GL_RGB8)
		{
			glGenTextures(1, &this->SRGBTextureView);
			glTextureView(this->SRGBTextureView, GL_TEXTURE_2D, this->TextureID, GL_SRGB8, 0, this->MipMapLevels, 0, 1);
		}
		this->Device->CheckErrors("Texture2D color-space view creation");
		this->ApplySpecification(Info.Specification);
		if (this->Name.size() > static_cast<usize>(std::numeric_limits<GLsizei>::max()))
			throw Texture2DError("Texture2D debug name exceeds OpenGL label limits");
		glObjectLabel(GL_TEXTURE, this->TextureID, static_cast<GLsizei>(this->Name.size()), this->Name.c_str());
		this->Device->CheckErrors("Texture2D object label");
		this->MakeResident();
	}
	catch (...)
	{
		this->Release();
		throw;
	}
}

Texture2D::~Texture2D()
{
	this->Release();
}

Texture2D::Texture2D(Texture2D &&Other) noexcept
	: Device(std::exchange(Other.Device, nullptr)), Name(std::move(Other.Name)), Width(std::exchange(Other.Width, 0)),
	  Height(std::exchange(Other.Height, 0)), Channels(std::exchange(Other.Channels, 0)),
	  TextureHandle(std::exchange(Other.TextureHandle, 0)), SRGBTextureHandle(std::exchange(Other.SRGBTextureHandle, 0)),
	  TextureID(std::exchange(Other.TextureID, 0)), SRGBTextureView(std::exchange(Other.SRGBTextureView, 0)),
	  Resident(std::exchange(Other.Resident, false)), Locked(std::exchange(Other.Locked, false)),
	  MipMapLevels(std::exchange(Other.MipMapLevels, 0))
{
}

Texture2D &Texture2D::operator=(Texture2D &&Other) noexcept
{
	if (this != &Other)
	{
		this->Release();
		this->Device = std::exchange(Other.Device, nullptr);
		this->Name = std::move(Other.Name);
		this->Width = std::exchange(Other.Width, 0);
		this->Height = std::exchange(Other.Height, 0);
		this->Channels = std::exchange(Other.Channels, 0);
		this->TextureHandle = std::exchange(Other.TextureHandle, 0);
		this->SRGBTextureHandle = std::exchange(Other.SRGBTextureHandle, 0);
		this->TextureID = std::exchange(Other.TextureID, 0);
		this->SRGBTextureView = std::exchange(Other.SRGBTextureView, 0);
		this->Resident = std::exchange(Other.Resident, false);
		this->Locked = std::exchange(Other.Locked, false);
		this->MipMapLevels = std::exchange(Other.MipMapLevels, 0);
	}
	return *this;
}

std::string_view Texture2D::GetName() const noexcept
{
	return this->Name;
}
GLuint64 Texture2D::GetHandle() const noexcept
{
	return this->TextureHandle;
}
GLuint64 Texture2D::GetHandle(const TextureColorSpace ColorSpace) const
{
	this->RequireUsable();
	if (!this->Resident)
		throw Texture2DError("Texture2D bindless handle requested before residency");
	if (ColorSpace == TextureColorSpace::SRGB)
	{
		if (this->SRGBTextureView == 0 || this->SRGBTextureHandle == 0)
			throw Texture2DError("Texture2D storage format does not support an sRGB sampling view");
		return this->SRGBTextureHandle;
	}
	return this->TextureHandle;
}
GLuint Texture2D::GetTextureID() const noexcept
{
	return this->TextureID;
}

pipeline::device::Device &Texture2D::GetDevice() const
{
	this->RequireUsable();
	return *this->Device;
}

void Texture2D::MakeResident()
{
	this->RequireUsable();
	if (this->Resident)
		return;
	this->Device->CheckErrors("Texture2D::makeResident precondition");
	this->TextureHandle = glGetTextureHandleARB(this->TextureID);
	glMakeTextureHandleResidentARB(this->TextureHandle);
	if (this->SRGBTextureView != 0)
	{
		this->SRGBTextureHandle = glGetTextureHandleARB(this->SRGBTextureView);
		glMakeTextureHandleResidentARB(this->SRGBTextureHandle);
	}
	this->Device->CheckErrors("Texture2D bindless residency");
	this->Resident = true;
}

void Texture2D::MakeNonResident()
{
	this->RequireUsable();
	if (!this->Resident)
		return;
	if (this->Locked)
		throw Texture2DError("Cannot make a locked Texture2D non-resident");
	this->Device->CheckErrors("Texture2D::makeNonResident precondition");
	if (this->SRGBTextureHandle != 0)
		glMakeTextureHandleNonResidentARB(this->SRGBTextureHandle);
	glMakeTextureHandleNonResidentARB(this->TextureHandle);
	this->Device->CheckErrors("Texture2D bindless non-residency");
	this->Resident = false;
	this->TextureHandle = 0;
	this->SRGBTextureHandle = 0;
}

bool Texture2D::IsResident() const noexcept
{
	return this->Resident;
}
bool Texture2D::IsLocked() const noexcept
{
	return this->Locked;
}
int32 Texture2D::GetWidth() const noexcept
{
	return this->Width;
}
int32 Texture2D::GetHeight() const noexcept
{
	return this->Height;
}
int32 Texture2D::GetChannels() const noexcept
{
	return this->Channels;
}
int32 Texture2D::GetMipMapLevels() const noexcept
{
	return this->MipMapLevels;
}
bool Texture2D::CanModify() const noexcept
{
	return this->TextureID != 0 && !this->Resident && !this->Locked;
}

void Texture2D::ApplySpecification(const Texture2DSpecification &Info)
{
	this->RequireUsable();
	if (!this->CanModify())
		throw Texture2DError("Texture2D specification cannot change while resident or locked");
	if (Info.Anisotropy < 1.0f)
		throw Texture2DError("Texture2D anisotropy must be at least one");
	this->ApplySpecificationTo(this->TextureID, Info);
	if (this->SRGBTextureView != 0)
		this->ApplySpecificationTo(this->SRGBTextureView, Info);
	this->Device->CheckErrors("Texture2D sampler specification");
}

void Texture2D::ApplySpecificationTo(const GLuint Texture, const Texture2DSpecification &Info)
{
	glTextureParameteri(Texture, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(Info.MinFilter));
	glTextureParameteri(Texture, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(Info.MagFilter));
	glTextureParameteri(Texture, GL_TEXTURE_WRAP_S, static_cast<GLint>(Info.WrapS));
	glTextureParameteri(Texture, GL_TEXTURE_WRAP_T, static_cast<GLint>(Info.WrapT));
	if (this->Device->SupportsExtension("GL_EXT_texture_filter_anisotropic"))
	{
		glTextureParameterf(Texture, GL_TEXTURE_MAX_ANISOTROPY_EXT,
							std::min(Info.Anisotropy, this->Device->GetCapabilities().MaximumAnisotropy));
	}
}

void Texture2D::Lock() noexcept
{
	this->Locked = true;
}
void Texture2D::Unlock() noexcept
{
	this->Locked = false;
}

void Texture2D::RequireUsable() const
{
	if (this->Device == nullptr || this->TextureID == 0)
		throw Texture2DError("Texture2D does not own a GPU texture");
	(void)this->Device->RequireCurrentContext();
}

void Texture2D::Release() noexcept
{
	if (this->TextureID == 0)
		return;
	const bool CanReleaseGPUResource = this->Device != nullptr && this->Device->CanIssueCommands();
	if (this->Resident && CanReleaseGPUResource)
	{
		if (this->SRGBTextureHandle != 0)
			glMakeTextureHandleNonResidentARB(this->SRGBTextureHandle);
		if (this->TextureHandle != 0)
			glMakeTextureHandleNonResidentARB(this->TextureHandle);
	}
	if (this->SRGBTextureView != 0 && CanReleaseGPUResource)
		glDeleteTextures(1, &this->SRGBTextureView);
	if (CanReleaseGPUResource)
		glDeleteTextures(1, &this->TextureID);
	this->TextureID = 0;
	this->SRGBTextureView = 0;
	this->TextureHandle = 0;
	this->SRGBTextureHandle = 0;
	this->Resident = false;
	this->Locked = false;
}
} // namespace renderer::texture
