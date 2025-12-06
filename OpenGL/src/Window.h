#pragma once
#include <string>
#include <GLFW/glfw3.h>
#include <unordered_map>
#include <functional>

class Window final {
private:
	GLFWwindow* window;
	std::string title;
	unsigned int height;
	unsigned int width;
	double mouseX;
	double mouseY;
	double deltaMouseX;
	double deltaMouseY;
	bool isFullscreen;
	using keymap = std::unordered_map<int, std::function<void(int)>>;
	keymap keyPressedEvents;
	keymap keyReleasedEvents;
	bool keysPressed[GLFW_KEY_LAST];
	bool mouseButtonsPressed[GLFW_MOUSE_BUTTON_LAST];
	static inline Window* windowPtr = nullptr;
	float frameDeltaTime;
public:
	Window(const std::string&, unsigned int, unsigned int);
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