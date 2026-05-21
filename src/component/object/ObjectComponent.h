#pragma once
#include "src/scene/Object.h"
#include "src/Types.h"

namespace components
{
	struct ComponentTypeIDSystem
	{
		inline static std::size_t nextID = 0;


		template<typename T>
		inline static std::size_t getNewTypeID()
		{
			nextID++;
			return nextID;
		}
	};

	class ObjectComponent
	{
	private:
		world::Object* object;
	public:
		ObjectComponent() = delete;
		virtual ~ObjectComponent() = 0;

		world::Object* getOwnerPointer() const
		{
			return this->object;
		}

		template<typename T>
		inline static std::size_t getTypeID()
		{
			std::size_t typeID = ComponentTypeIDSystem::getNewTypeID<T>();
			return typeID++;
		}

		virtual void onAttachment() {}
		virtual void onDetachment() {}
		virtual void onRender() {};
		virtual void onUpdate() {};
	};

	inline components::ObjectComponent::~ObjectComponent() {}
}