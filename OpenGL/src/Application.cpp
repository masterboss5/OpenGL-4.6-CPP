#include "Application.h"

namespace core
{
	void Application::run()
	{
		this->running = true;
		while (this->running)
		{
			for (const auto& layer : this->layers)
			{
				layer->update();
			}

			for (const auto& layer : this->layers)
			{
				layer->render();
			}
		}
	}

	void Application::stop()
	{
		this->running = false;
	}

	void Application::pushLayer(std::unique_ptr<ApplicationLayer> layer)
	{
		this->layers.push_back(std::move(layer));
	}

	size_t Application::getLayerCount() const
	{
		return this->layers.size();
	}
}
