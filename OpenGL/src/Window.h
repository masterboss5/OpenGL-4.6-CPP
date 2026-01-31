#pragma once
#include <string>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <unordered_map>
#include <functional>
// Forward-declare WindowSpecification to avoid circular include with Application.h
namespace core { struct WindowSpecification; }

class Window final {
private:
	std::string title;
	unsigned int height;
	unsigned int width;
	double mouseX = 0;
	double mouseY = 0;
	double deltaMouseX = 0;
	double deltaMouseY = 0;
	bool isFullscreen = false;
	using keymap = std::unordered_map<int, std::function<void(int)>>;
	keymap keyPressedEvents = {};
	keymap keyReleasedEvents = {};
	bool keysPressed[GLFW_KEY_LAST] = {};
	bool mouseButtonsPressed[GLFW_MOUSE_BUTTON_LAST] = {};
	
	float frameDeltaTime = 0.0f;
public:
	GLFWwindow* window = nullptr;
	static inline Window* windowPtr = nullptr;

	Window(core::WindowSpecification windowSpecification);
	Window(std::string title, int width, int height);
	~Window();
	Window(const Window&) = default;
	Window& operator=(const Window&) = default;
	Window(Window&&) = delete;
	Window& operator=(Window&&) = delete;

	void setupInputs();
	void registerKeyPressEvent(int key, std::function<void(int key)>);
	void registerKeyReleaseEvent(int key, std::function<void(int key)>);
	void onKeyPress(GLFWwindow* glfwWindow, int key, int scancode, int mods);
	void onKeyRelease(GLFWwindow* glfwWindow, int key, int scancode, int mods);
	void onMouseClick(int, int);
	void onMouseMove(double, double);
	void onWindowResize(int, int);
	double getMouseX() const;
	double getMouseY() const;
	double getDeltaMouseX() const;
	double getDeltaMouseY() const;
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
	bool isKeyPressed(int key) const;
	void lockMouse();
	float getFrameDeltaTime() const;
};