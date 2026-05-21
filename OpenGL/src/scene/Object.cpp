#include "Object.h"

world::Object::Object()
{
}

world::Object::~Object()
{
	for (auto component : this->components)
	{
		if (component != nullptr)
		{
			component->onDetachment();
			delete component;
		}
	}
}

uint32 world::Object::getComponentsAttached() const
{
	return this->componentsAttached;
}
