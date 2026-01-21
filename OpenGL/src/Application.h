#pragma once
#include<memory>
#include<vector>
#include "ApplicationLayer.h"
#include "Window.h"

namespace core
{
	class Application final
	{
	private:
		bool running = false;
		std::vector<std::unique_ptr<ApplicationLayer>> layers;
	public:
		void run();
		void stop();
		void pushLayer(std::unique_ptr<ApplicationLayer> layer);
		size_t getLayerCount() const;
	};
}