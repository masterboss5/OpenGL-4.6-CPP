#include "RenderLayer.h"
#include <iostream>


RenderLayer::~RenderLayer()
{
	std::cout << "RenderLayer::~RenderLayer()" << std::endl;
}

void RenderLayer::render()
{
	std::cout << "RenderLayer::render()" << std::endl;
}

void RenderLayer::update()
{

	std::cout << "RenderLayer::update()" << std::endl;
}
