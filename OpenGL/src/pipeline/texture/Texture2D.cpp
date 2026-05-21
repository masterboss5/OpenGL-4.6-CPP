#include "Texture2D.h"

namespace renderer::texture
{
	namespace //internal file symbols
	{
		int32 calculateMipMapLevels(int32 width, int32 height)
		{
			int32 maxDimension = std::max(width, height);
			int32 levels = static_cast<int32>(std::floor(std::log2(maxDimension))) + 1;

			return levels;
		}
	}

	static uint32 TEXTURE2D_INSTANCE_COUNT = 0;
	static uint32 TEXTURE2D_CONSTRUCTUED_COUNT = 0;
	static uint32 TEXTURE2D_DECSTRUCTUED_COUNT = 0;

	Texture2DSpecification const Texture2DSpecification::DEFAULT_INSTANCE =
	{
		.internalFormat = GL_RGBA8,
		.dataFormat = GL_RGBA,
		.dataType = GL_UNSIGNED_BYTE,
		.minFilter = GL_LINEAR_MIPMAP_LINEAR,
		.magFilter = GL_LINEAR,
		.wrapS = GL_REPEAT,
		.wrapT = GL_REPEAT,
		.anisotropy = 16.0f
	};
}

renderer::texture::Texture2D::Texture2D(std::string name, Texture2DCreationInfo& info)
	: Asset(util::UUID::generateRandomUUID()),
	name(name),
	width(info.width),
	height(info.height),
	channels(info.channels)
{
	glCreateTextures(GL_TEXTURE_2D, 1, &textureID);
	this->mipMapLevels = renderer::texture::calculateMipMapLevels(this->width, this->height);
	glTextureStorage2D(this->textureID, this->mipMapLevels, info.specification.internalFormat, this->width, this->height);
	glTextureSubImage2D(this->textureID, 0, 0, 0, this->width, this->height, info.specification.dataFormat, info.specification.dataType, info.pixels);
	glGenerateTextureMipmap(this->textureID);
	this->applySpecification(info.specification);
	this->makeResident();
}

renderer::texture::Texture2D::Texture2D(Texture2D&& other) noexcept
	: Asset(std::move(other)),
	name(std::move(other.name)),
	width(other.width),
	height(other.height),
	channels(other.channels),
	textureHandle(other.textureHandle),
	textureID(other.textureID),
	resident(other.resident),
	locked(other.locked),
	mipMapLevels(other.mipMapLevels)
{
	other.makeNonResident();
	other.textureID = 0;
	other.textureHandle = 0;
	other.lock();
}
	
renderer::texture::Texture2D& renderer::texture::Texture2D::operator=(Texture2D&& other) noexcept
{
	if (this != &other)
	{
		if (this->getTextureID() != 0)
		{
			this->makeNonResident();
			glDeleteTextures(1, &this->textureID);
		}

		this->name = std::move(other.name);
		this->width = other.width;
		this->height = other.height;
		this->channels = other.channels;
		this->textureHandle = other.textureHandle;
		this->textureID = other.textureID;
		this->resident = other.resident;
		this->locked = other.locked;
		this->mipMapLevels = other.mipMapLevels;
		other.makeNonResident();
		other.textureID = 0;
		other.textureHandle = 0;
		other.lock();
	}

	return *this;
}

renderer::texture::Texture2D::~Texture2D()
{
	if (this->getTextureID() != 0)
	{
		this->makeNonResident();
		glDeleteTextures(1, &this->textureID);
	}
}

std::string_view renderer::texture::Texture2D::getName() const
{
	return this->name;
}

GLuint64 renderer::texture::Texture2D::getHandle() const
{
	return this->textureHandle;
}

GLuint renderer::texture::Texture2D::getTextureID() const
{
	return this->textureID;
}

void renderer::texture::Texture2D::makeResident()
{
	if (!this->isResident())
	{
		this->textureHandle = glGetTextureHandleARB(this->textureID);
		glMakeTextureHandleResidentARB(this->textureHandle);
		this->resident = true;
	}
}

void renderer::texture::Texture2D::makeNonResident()
{
	if (this->isResident() && !this->isLocked())
	{
		glMakeTextureHandleNonResidentARB(this->textureHandle);
		this->resident = false;
	}
}

bool renderer::texture::Texture2D::isResident() const
{
	return this->resident;
}

bool renderer::texture::Texture2D::isLocked() const
{
	return this->locked;
}

int32 renderer::texture::Texture2D::getWidth() const
{
	return this->width;
}

int32 renderer::texture::Texture2D::getHeight() const
{
	return this->height;
}

int32 renderer::texture::Texture2D::getChannels() const
{
	return this->channels;
}

int32 renderer::texture::Texture2D::getMipMapLevels() const
{
	return this->mipMapLevels;
}

bool renderer::texture::Texture2D::canModify() const
{
	return !this->resident && !this->locked;
}

void renderer::texture::Texture2D::applySpecification(Texture2DSpecification& info)
{
	if (this->canModify())
	{
		glTextureParameteri(this->textureID, GL_TEXTURE_MIN_FILTER, info.minFilter);
		glTextureParameteri(this->textureID, GL_TEXTURE_MAG_FILTER, info.magFilter);
		glTextureParameteri(this->textureID, GL_TEXTURE_WRAP_S, info.wrapS);
		glTextureParameteri(this->textureID, GL_TEXTURE_WRAP_T, info.wrapT);
		glTextureParameterf(this->textureID, GL_TEXTURE_MAX_ANISOTROPY, info.anisotropy);
	}
}

void renderer::texture::Texture2D::lock()
{
	this->locked = true;
}

void renderer::texture::Texture2D::unlock()
{
	this->locked = false;
}