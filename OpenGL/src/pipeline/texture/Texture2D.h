#pragma once
#include <cstdint>
#include "src/types.h"
#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include "src/resource/Asset.h"

namespace renderer::texture
{
	struct Texture2DSpecification final
	{
		static const Texture2DSpecification DEFAULT_INSTANCE;
		GLenum internalFormat;
		GLenum dataFormat;
		GLenum dataType;
		GLenum minFilter;
		GLenum magFilter;
		GLenum wrapS;
		GLenum wrapT;
		float anisotropy;
	};

	struct Texture2DCreationInfo final
	{
		uint8* pixels;
		int32 width;
		int32 height;
		int32 channels;
		Texture2DSpecification specification;
	};

	class Texture2D final : public resource::Asset
	{
	private:
		std::string name;
		int32 width;
		int32 height;
		int32 channels;
		GLuint64 textureHandle;
		GLuint textureID;
		bool resident = false;
		bool locked = false;
		int32 mipMapLevels;
	public:
		explicit Texture2D(std::string name, Texture2DCreationInfo& info);
		Texture2D(Texture2D& other) noexcept = delete;
		Texture2D& operator=(Texture2D& other) noexcept = delete;
		Texture2D(Texture2D&& other) noexcept;
		Texture2D& operator=(Texture2D&& other) noexcept;

		~Texture2D();

		std::string_view getName() const;
		GLuint64 getHandle() const;
		GLuint getTextureID() const;
		void makeResident();
		void makeNonResident();
		bool isResident() const;
		bool isLocked() const;
		int32 getWidth() const;
		int32 getHeight() const;
		int32 getChannels() const;
		int32 getMipMapLevels() const;
		bool canModify() const;
		void applySpecification(Texture2DSpecification& info);
		void lock();
		void unlock();
	};
}