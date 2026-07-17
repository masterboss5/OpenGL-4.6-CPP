#pragma once
#include <string>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <unordered_map>
#include <functional>
#include "src/types.h"

namespace core
{
	struct WindowSpecification;
}

class Window final {
private:
	std::string title;
	uint32 height;
	uint32 width;
	bool isFullscreen = false;	
	float32 frameDeltaTime = 0.0f;

	static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
	void setFramebufferExtent(uint32 width, uint32 height);
public:
	GLFWwindow* window = nullptr;
	static inline Window* windowPtr = nullptr;

	Window(core::WindowSpecification windowSpecification);
	~Window();
	Window(const Window&) = default;
	Window& operator=(const Window&) = default;
	Window(Window&&) = delete;
	Window& operator=(Window&&) = delete;

	void swapBuffers();
	void updateViewport();
	void setWidth(uint32 width);
	uint32 getWidth() const;	
	void setHeight(uint32 height);
	uint32 getHeight() const;
	float32 getAspectRatio() const;
	void setTitle(const std::string&);
	std::string getTitle();
	bool getIsFullscreen();
	bool shouldClose();
	void closeWindow() const;
	void makeContextCurrent();
	void pollEvents();
	void clearColor(); //TODO: parameters
	void uncapFPS();
	float32 getFrameDeltaTime() const;
};
