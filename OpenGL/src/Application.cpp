#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Application.h"

namespace core
{
	Application::Application(WindowSpecification windowSpecification)
	{
		glfwInit();
		this->window = std::make_unique<Window>(windowSpecification);
		this->window->makeContextCurrent();
		this->window->lockMouse();
		this->window->setupInputs();
		this->window->uncapFPS();
		glewInit();
	}

	void Application::run()
	{
		this->running = true;
		while (this->running)
		{
			this->running = !this->window->shouldClose();
			core::input::InputManager::getInstance()->update();
			this->window->pollEvents();
			this->window->clearColor();
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glEnable(GL_DEPTH_TEST);

			for (const auto& layer : this->layers)
			{
				layer->run();
			}

			this->window->swapBuffers();
		}
	}

	void Application::stop()
	{
		this->running = false;
	}

	size_t Application::getLayerCount() const
	{
		return this->layers.size();
	}
}
