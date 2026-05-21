#pragma once

class ApplicationLayer
{
public:
	virtual ~ApplicationLayer() = default;
	virtual void run() = 0;
};