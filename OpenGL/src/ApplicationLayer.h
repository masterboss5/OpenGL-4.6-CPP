#pragma once

class ApplicationLayer
{
public:
	virtual ~ApplicationLayer() = default;
	virtual void render() = 0;
	virtual void update() = 0;
};