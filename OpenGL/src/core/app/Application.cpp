#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <stdexcept>
#include "Application.h"
#include "src/pipeline/device/OpenGLRuntime.h"
#include "src/renderer/RenderCoreValidation.h"

namespace core
{
	Application::Application(WindowSpecification windowSpecification)
	{
		glfwInit();
		if (pipeline::device::isHeadlessPresentationValidationEnabled()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
		glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
		this->window = std::make_unique<Window>(windowSpecification);
		this->window->makeContextCurrent();
		this->window->uncapFPS();
		glewExperimental = GL_TRUE;
		if (glewInit() != GLEW_OK)
		{
			throw std::runtime_error("Failed to initialize the OpenGL 4.6 function dispatch");
		}
		glGetError(); // GLEW may leave a harmless GL_INVALID_ENUM behind.
		pipeline::device::configureDebugOutput();
		glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
		glClearDepth(0.0);
		glDepthFunc(GL_GREATER);
		if (pipeline::device::isDeterministicRenderCoreValidationEnabled()) renderer::validation::runDeterministicRenderCoreChecks();
		core::input::InputManager::getInstance()->lockMouse();
	}

	void Application::main()
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
			glDepthFunc(GL_GREATER);

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
