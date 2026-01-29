#include "Application.h"

namespace core
{
	Application::Application(WindowSpecification windowSpecification)
		: running(false)
	{
		//this->window = std::make_unique<Window>()
	}

	void Application::run()
	{
		//this->running = true;
		//while (this->running)
		//{
		//	for (const auto& layer : this->layers)
		//	{
		//		layer->update();
		//	}

		//	for (const auto& layer : this->layers)
		//	{
		//		layer->render();
		//	}
		//}
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
