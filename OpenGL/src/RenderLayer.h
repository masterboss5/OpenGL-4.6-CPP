#pragma once
#include "ApplicationLayer.h"

class RenderLayer final : public ApplicationLayer
{
public:
	virtual ~RenderLayer() override;
	virtual void render() override;
	virtual void update() override;
};