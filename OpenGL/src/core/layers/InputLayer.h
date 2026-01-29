#pragma once
#include "src/ApplicationLayer.h"
#include"src/core/input/InputManager.h"
#include <memory>

namespace core
{
	class InputLayer final : public ApplicationLayer
	{
	public:
		InputLayer()
		{
			core::input::InputManager::getInstance();
		}

		virtual ~InputLayer() override
		{
		}

		virtual void render() override
		{
		};

		virtual void update() override
		{
			core::input::InputManager::getInstance()->update();
		};
	};
}