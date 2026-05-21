#pragma once
#include <string>
#include "stb_image.h"
#include <GL/glew.h>
#include <iostream>

class Texture final
{
private:
	unsigned long long textureHandle;
	unsigned int textureID;
	int width;
	int height;
	int channels;
	unsigned char* pixels;
public:
	Texture(const std::string& path);
	~Texture();

	int getWidth() const;
	int getHeight() const;
	unsigned long long getHandle() const;
	unsigned int size() const;
	bool isLoaded() const;
};