#include "Window.h"
#include "Application.h"
#include <iostream>

Window::Window(core::WindowSpecification windowSpecification)
	: title(windowSpecification.windowTitle), width(windowSpecification.width), height(windowSpecification.height)
{
	this->window = glfwCreateWindow(width, height, this->title.c_str(), nullptr, nullptr);
	glfwMakeContextCurrent(this->window);
	windowPtr = this;
}

Window::~Window()
{
	glfwDestroyWindow(this->window);
	this->window = nullptr;
}

void Window::setupInputs()
{
	//glfwSetKeyCallback(this->window, [](GLFWwindow* glfwWindow, int key, int scancode, int action, int mods) {
	//	switch (action) {
	//	case GLFW_PRESS:
	//		windowPtr->keysPressed[key] = true;

	//		if (windowPtr->keyPressedEvents.contains(key)) {
	//			windowPtr->keyPressedEvents[key](key);
	//			windowPtr->onKeyPress(glfwWindow, key, scancode, mods);
	//		}
	//		break;

	//	case GLFW_RELEASE:
	//		windowPtr->keysPressed[key] = false;

	//		if (windowPtr->keyReleasedEvents.contains(key)) {
	//			windowPtr->keyReleasedEvents[key](key);
	//			windowPtr->onKeyRelease(glfwWindow, key, scancode, mods);
	//		}
	//		break;

	//	case GLFW_REPEAT:
	//		windowPtr->keysPressed[key] = true;

	//		if (windowPtr->keyPressedEvents.contains(key)) {
	//			windowPtr->keyPressedEvents[key](key);
	//			windowPtr->onKeyPress(glfwWindow, key, scancode, mods);
	//		}
	//		break;
	//	}
	//});

	//glfwSetCursorPosCallback(this->window, [](GLFWwindow* glfwWindow, double mouseX, double mouseY) {
	//	windowPtr->deltaMouseX = mouseX - windowPtr->mouseX;
	//	windowPtr->deltaMouseY = mouseY - windowPtr->mouseY;
	//	windowPtr->mouseX = mouseX;
	//	windowPtr->mouseY = mouseY;
	//	windowPtr->onMouseMove(mouseX, mouseY);
	//});

	//glfwSetMouseButtonCallback(this->window, [](GLFWwindow* glfwWindow, int button, int action, int mods) {
	//	windowPtr->mouseButtonsPressed[button] = (action != GLFW_RELEASE);
	//	windowPtr->onMouseClick(button, action);
	//});

	//glfwSetWindowSizeCallback(this->window, [](GLFWwindow* glfwWindow, int width, int height) {
	//	windowPtr->setWidth(width);
	//	windowPtr->setHeight(height);
	//	windowPtr->onWindowResize(width, height);
	//});
}

void Window::onMouseClick(int button, int action) {
	if (action == GLFW_PRESS) {
		std::cout << "click;";
	} else if (action == GLFW_RELEASE) {

	}
}

void Window::onMouseMove(double mouseX, double mouseY) {

}

void Window::onWindowResize(int width, int height) {

}

double Window::getMouseX() const
{
	return this->mouseX;
}

double Window::getMouseY() const
{
	return this->mouseY;
}

double Window::getDeltaMouseX() const
{
	return this->deltaMouseX;
}

double Window::getDeltaMouseY() const
{
	return this->deltaMouseY;
}

void Window::registerKeyPressEvent(int key, std::function<void(int)> callback) {
	this->keyPressedEvents[key] = callback;
}

void Window::registerKeyReleaseEvent(int key, std::function<void(int)> callback) {
	this->keyReleasedEvents[key] = callback;
}

void Window::onKeyPress(GLFWwindow* glfwWindow, int key, int scancode, int mods)
{

}

void Window::onKeyRelease(GLFWwindow* glfwWindow, int key, int scancode, int mods) {

}

bool Window::isKeyPressed(int key) const {
	return this->keysPressed[key];
}

void Window::lockMouse()
{
	glfwSetInputMode(this->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

float Window::getFrameDeltaTime() const
{
	return this->frameDeltaTime;
}

void Window::swapBuffers() {
	glfwSwapBuffers(this->window);
}

bool Window::shouldClose() {
	return glfwWindowShouldClose(this->window);
}

void Window::closeWindow()
{
	glfwSetWindowShouldClose(this->window, true);
}

void Window::updateViewport() {
	glViewport(0, 0, this->width, this->height);
}

unsigned int Window::getHeight() {
	return this->height;
}

float Window::getAspectRatio() const
{
	return this->width / this->height;
}

void Window::setHeight(unsigned int height) {
	this->height = height;
	this->updateViewport();
}

unsigned int Window::getWidth() {
	return this->width;
}

void Window::setWidth(unsigned int width) {
	this->width = width;
	this->updateViewport();
}

std::string Window::getTitle() {
	return this->title;
}

void Window::setTitle(const std::string& name) {
	this->title = name;
	glfwSetWindowTitle(this->window, name.c_str());
}

bool Window::getIsFullscreen() {
	return this->isFullscreen;
}

void Window::makeContextCurrent() {
	glfwMakeContextCurrent(this->window);
}

float lastFrameTime = 0.0f;
void Window::pollEvents() {
	float currentTime = static_cast<float>(glfwGetTime());
	this->frameDeltaTime = currentTime - lastFrameTime;
	lastFrameTime = currentTime;

	this->deltaMouseX = 0.0;
	this->deltaMouseY = 0.0;
	glfwPollEvents();
}

void Window::clearColor() {
	//glClearColor(0.53f, 0.81f, 0.92f, 1.0f);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void Window::uncapFPS() {
	glfwSwapInterval(0);
}