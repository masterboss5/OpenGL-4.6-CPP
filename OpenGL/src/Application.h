#pragma once
#include<memory>
#include<vector>
#include "ApplicationLayer.h"
#include "Window.h"
#include "src/core/input/InputManager.h"
#include "src/Camera.h"
#include "src/OpenGLRenderer.h"

namespace core
{
	struct WindowSpecification final
	{
		std::string windowTitle;
		unsigned int width;
		unsigned int height;
	};

	class Application final
	{
	private:
		bool running = false;
		std::unique_ptr<Window> window = nullptr;
		std::vector<std::unique_ptr<ApplicationLayer>> layers = {};
	public:
		Application(WindowSpecification windowSpecification = {"Window", 100, 100});

		void run();
		void stop();
		size_t getLayerCount() const;

		template<typename TLayer>
		void pushLayer()
		{
			this->layers.push_back(std::make_unique<TLayer>(this->window.get()));
		}
	};
}