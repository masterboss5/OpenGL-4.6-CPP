#include "Window.h"
#include "src/core/app/Application.h"
#include <iostream>

Window::Window(core::WindowSpecification windowSpecification)
	: title(windowSpecification.windowTitle), width(windowSpecification.width), height(windowSpecification.height)
{
	this->window = glfwCreateWindow(width, height, this->title.c_str(), nullptr, nullptr);
	glfwMakeContextCurrent(this->window);
	glfwSetWindowUserPointer(this->window, this);
	glfwSetFramebufferSizeCallback(this->window, framebufferSizeCallback);

	int framebufferWidth = 0;
	int framebufferHeight = 0;
	glfwGetFramebufferSize(this->window, &framebufferWidth, &framebufferHeight);
	if (framebufferWidth > 0 && framebufferHeight > 0)
	{
		this->setFramebufferExtent(static_cast<uint32>(framebufferWidth), static_cast<uint32>(framebufferHeight));
	}
	windowPtr = this;
}

Window::~Window()
{
	if (windowPtr == this) windowPtr = nullptr;
	if (this->window != nullptr) glfwDestroyWindow(this->window);
	this->window = nullptr;
}

float32 Window::getFrameDeltaTime() const
{
	return this->frameDeltaTime;
}

void Window::swapBuffers() {
	glfwSwapBuffers(this->window);
}

bool Window::shouldClose() {
	return glfwWindowShouldClose(this->window);
}

void Window::closeWindow() const
{
	glfwSetWindowShouldClose(this->window, true);
}

void Window::updateViewport() {
	glViewport(0, 0, static_cast<GLsizei>(this->width), static_cast<GLsizei>(this->height));
}

uint32 Window::getHeight() const {
	return this->height;
}

float32 Window::getAspectRatio() const
{
	return this->height == 0 ? 0.0f : static_cast<float32>(this->width) / static_cast<float32>(this->height);
}

void Window::setHeight(uint32 height) {
	this->height = height;
	this->updateViewport();
}

uint32 Window::getWidth() const {
	return this->width;
}

void Window::setWidth(uint32 width) {
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

float32 lastFrameTime = 0.0f;
void Window::pollEvents() {
	const float32 currentTime = static_cast<float32>(glfwGetTime());
	this->frameDeltaTime = currentTime - lastFrameTime;
	lastFrameTime = currentTime;

	glfwPollEvents();
}

void Window::clearColor() {
	//glClearColor(0.53f, 0.81f, 0.92f, 1.0f);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void Window::uncapFPS() {
	glfwSwapInterval(0);
}

void Window::framebufferSizeCallback(GLFWwindow* window, int width, int height)
{
	auto* const instance = static_cast<Window*>(glfwGetWindowUserPointer(window));
	if (instance == nullptr || width <= 0 || height <= 0)
	{
		return;
	}

	instance->setFramebufferExtent(static_cast<uint32>(width), static_cast<uint32>(height));
}

void Window::setFramebufferExtent(uint32 width, uint32 height)
{
	this->width = width;
	this->height = height;
	this->updateViewport();
}
