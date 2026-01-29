#pragma once
#include "src/ApplicationLayer.h"
#include <memory>

namespace core
{
	class WindowLayer final : public ApplicationLayer
	{
	public:
		WindowLayer()
		{

		}

		virtual ~WindowLayer() override
		{
		}

		virtual void render() override
		{
		};

		virtual void update() override
		{
		};
	};
}