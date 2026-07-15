#pragma once
#include "src/component/Components.h"
#include "src/types.h"

namespace world
{
	class Object;
}

namespace components
{
	class CObjectComponent
	{
	private:
		world::Object* object = nullptr;
		bool enabled = true;

	public:
		CObjectComponent() = delete;
		explicit CObjectComponent(world::Object* object) : object(object) {}
		virtual ~CObjectComponent() = default;
		
		[[nodiscard]] bool hasOwner() const
		{
			return this->object != nullptr;
		}

		[[nodiscard]] world::Object* getOwner() const
		{
			return this->object;
		}

		[[nodiscard]] virtual uint32 getTypeID() const = 0;
		[[nodiscard]] virtual std::string_view getComponentName() const = 0;

		/*Lifecycle events and gameplay behavior*/

		virtual void onAttachment() {}
		virtual void onDetachment() {}
		virtual void onRender() {}
		virtual void onUpdate() {}
	};
}