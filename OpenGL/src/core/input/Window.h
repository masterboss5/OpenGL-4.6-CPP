#pragma once
#include <string>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <unordered_map>
#include <functional>

namespace core
{
	struct WindowSpecification;
}

class Window final {
private:
	std::string title;
	unsigned int height;
	unsigned int width;
	bool isFullscreen = false;	
	float frameDeltaTime = 0.0f;
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
	void setWidth(unsigned int);
	unsigned int getWidth();	
	void setHeight(unsigned int);
	unsigned int getHeight();
	float getAspectRatio() const;
	void setTitle(const std::string&);
	std::string getTitle();
	bool getIsFullscreen();
	bool shouldClose();
	void closeWindow();
	void makeContextCurrent();
	void pollEvents();
	void clearColor(); //TODO: parameters
	void uncapFPS();
	float getFrameDeltaTime() const;
};