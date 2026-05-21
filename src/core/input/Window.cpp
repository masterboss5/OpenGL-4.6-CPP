#include "Window.h"
#include "src/core/app/Application.h"
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

	glfwPollEvents();
}

void Window::clearColor() {
	//glClearColor(0.53f, 0.81f, 0.92f, 1.0f);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void Window::uncapFPS() {
	glfwSwapInterval(0);
}