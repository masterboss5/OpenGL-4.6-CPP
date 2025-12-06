#include "Texture.h"
#include "stb_image.h"
#include <iostream>

Texture::Texture(const std::string& path) {
	this->pixels = stbi_load(path.c_str(), &this->width, &this->height, &this->channels, 4);
	std::cout << "Loaded texture: " << path << " (" << this->width << "x" << this->height << ", " << this->channels << " channels)" << std::endl;

	if (!this->pixels) {
		std::cerr << "[Texture] stbi_load failed for '" << path << "': " << stbi_failure_reason() << "\n";
		this->textureHandle = 0;
		this->textureID = 0;
		return;
	}

	glGenTextures(1, &this->textureID);
	glBindTexture(GL_TEXTURE_2D, this->textureID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, this->width, this->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, this->pixels);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	this->textureHandle = glGetTextureHandleARB(this->textureID);
	glMakeTextureHandleResidentARB(this->textureHandle);
}

Texture::~Texture() {
}

int Texture::getWidth() const {
	return this->width;
}

int Texture::getHeight() const {
	return this->height;
}

unsigned int Texture::size() const {
	return this->width * this->height * this->channels;
}

bool Texture::isLoaded() const {
	return this->textureHandle != 0 && this->textureID != 0;
}

unsigned long long Texture::getHandle() const {
	return this->textureHandle;
}