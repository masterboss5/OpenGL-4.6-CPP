#pragma once
#include "src/component/Components.h"
#include "src/Types.h"

#define CCOMPONENT_BODY(type) \
	static_assert(DependenciesRegistered<GetDependencies<type>, ComponentTypeList>::value, \
		#type " declares a dependency on an unregistered component type"); \
	inline static constexpr uint32 TYPE_ID = \
		static_cast<uint32>(ComponentTypeID<type>); \
	std::string_view getComponentName() const override final \
	{ \
		return #type; \
	} \
	\
	virtual uint32 getTypeID() const override final \
	{ \
		return TYPE_ID; \
	} \

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
	public:
		CObjectComponent() = delete;
		explicit CObjectComponent(world::Object* object) : object(object) {}
		virtual ~CObjectComponent() = default;
		
		[[nodiscard]] bool hasOwner() const
		{
			return this->object != nullptr;
		}

		[[nodiscard]] world::Object* getOwnerPointer() const
		{
			return this->object;
		}

		[[nodiscard]] virtual uint32 getTypeID() const = 0;
		[[nodiscard]] virtual std::string_view getComponentName() const = 0;
		virtual void onAttachment() {}
		virtual void onDetachment() {}
		virtual void onRender() {}
		virtual void onUpdate() {}
	};
}