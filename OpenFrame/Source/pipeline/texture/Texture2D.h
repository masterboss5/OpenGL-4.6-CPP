#pragma once

#include "Source/pipeline/device/Device.h"
#include "Source/types.h"

#include <GL/glew.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pipeline::texture
{
enum class TextureColorSpace : uint8
{
	Linear,
	SRGB
};

struct ENGINE_API Texture2DSpecification final
{
	static const Texture2DSpecification DefaultInstance;
	GLenum InternalFormat = GL_RGBA8;
	GLenum DataFormat = GL_RGBA;
	GLenum DataType = GL_UNSIGNED_BYTE;
	GLenum MinFilter = GL_LINEAR_MIPMAP_LINEAR;
	GLenum MagFilter = GL_LINEAR;
	GLenum WrapS = GL_REPEAT;
	GLenum WrapT = GL_REPEAT;
	float32 Anisotropy = 1.0f;
};

struct Texture2DCreationInfo final
{
	const uint8 *Pixels = nullptr;
	usize PixelBytes = 0;
	usize RowStrideBytes = 0;
	int32 Width = 0;
	int32 Height = 0;
	int32 Channels = 0;
	Texture2DSpecification Specification;
};

class ENGINE_API Texture2DError final : public std::runtime_error
{
  public:
	explicit Texture2DError(const std::string &Diagnostic) : std::runtime_error(Diagnostic)
	{
	}
};

class ENGINE_API Texture2D final
{
  public:
	Texture2D(pipeline::device::Device &Device, std::string Name, const Texture2DCreationInfo &Info);
	~Texture2D();

	Texture2D(const Texture2D &) = delete;
	Texture2D &operator=(const Texture2D &) = delete;
	Texture2D(Texture2D &&Other) noexcept;
	Texture2D &operator=(Texture2D &&Other) noexcept;

	[[nodiscard]] std::string_view GetName() const noexcept;
	[[nodiscard]] GLuint64 GetHandle() const noexcept;
	[[nodiscard]] GLuint64 GetHandle(TextureColorSpace ColorSpace) const;
	[[nodiscard]] GLuint GetTextureID() const noexcept;
	[[nodiscard]] pipeline::device::Device &GetDevice() const;
	void MakeResident();
	void MakeNonResident();
	[[nodiscard]] bool IsResident() const noexcept;
	[[nodiscard]] bool IsLocked() const noexcept;
	[[nodiscard]] int32 GetWidth() const noexcept;
	[[nodiscard]] int32 GetHeight() const noexcept;
	[[nodiscard]] int32 GetChannels() const noexcept;
	[[nodiscard]] int32 GetMipMapLevels() const noexcept;
	[[nodiscard]] bool CanModify() const noexcept;
	void ApplySpecification(const Texture2DSpecification &Info);
	void Lock() noexcept;
	void Unlock() noexcept;

  private:
	pipeline::device::DeviceHandle Device;
	std::string Name;
	int32 Width = 0;
	int32 Height = 0;
	int32 Channels = 0;
	GLuint64 TextureHandle = 0;
	GLuint64 SRGBTextureHandle = 0;
	GLuint TextureID = 0;
	GLuint SRGBTextureView = 0;
	bool Resident = false;
	bool Locked = false;
	int32 MipMapLevels = 0;

	void RequireUsable() const;
	void ApplySpecificationTo(GLuint Texture, const Texture2DSpecification &Info);
	void Release() noexcept;
};
} // namespace pipeline::texture
