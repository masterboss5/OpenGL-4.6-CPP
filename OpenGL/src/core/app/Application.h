#pragma once
#include<memory>
#include<vector>
#include "src/types.h"
#include "src/core/layers/ApplicationLayer.h"
#include "src/core/input/Window.h"
#include "src/core/input/InputManager.h"
#include "src/scene/Camera.h"
#include "src/renderer/OpenGLRenderer.h"

namespace core
{
	struct WindowSpecification final
	{
		std::string windowTitle;
		uint32 width;
		uint32 height;
	};

	class Application final
	{
	private:
		bool running = false;
		std::unique_ptr<Window> window = nullptr;
		std::vector<std::unique_ptr<ApplicationLayer>> layers = {};
	public:
		Application(WindowSpecification windowSpecification = {"WindowDefault", 100, 100});

		void main();
		void stop();
		size_t getLayerCount() const;

		template<typename TLayer>
		void pushLayer()
		{
			this->layers.push_back(std::make_unique<TLayer>(this->window.get()));
		}
	};
}