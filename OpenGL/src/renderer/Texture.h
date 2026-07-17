#pragma once
#include <string>
#include "stb_image.h"
#include <GL/glew.h>
#include <iostream>

class Texture final
{
private:
	unsigned long long textureHandle = 0;
	unsigned int textureID = 0;
	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char* pixels = nullptr;
public:
	Texture(const std::string& path);
	~Texture();

	int getWidth() const;
	int getHeight() const;
	unsigned long long getHandle() const;
	unsigned int size() const;
	bool isLoaded() const;
};
