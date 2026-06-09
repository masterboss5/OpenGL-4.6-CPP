#pragma once
#include<array>
#include "src/Types.h"
#include "src/concepts.h"
#include "src/component/object/CObjectComponent.h"
#include "src/component/Components.h"
#include <utility>

namespace world
{
	class Object
	{
	private:
		uint32 componentsAttached {};
		std::array<components::CObjectComponent*, components::COBJECT_COMPONENTS> components {};
	public:
		explicit Object();
		virtual ~Object();

		Object(Object&) = delete;
		Object& operator=(Object&) = delete;
		Object(Object&&) = delete;
		Object& operator=(Object&&) = delete;

		template<typename T, typename... Args> requires isCObjectComponent<T>
		void addComponent(Args&&... args)
		{
			constexpr std::size_t TYPE_ID = T::TYPE_ID;
			static_assert(TYPE_ID < components::COBJECT_COMPONENTS,
				"Object component type id exceeded component storage");

			if (this->components[TYPE_ID] != nullptr)
			{
				return;
			}

			//TODO later add better allocator
			T* newComponent = new T(this, std::forward<Args>(args)...);
			this->components[TYPE_ID] = newComponent;
			this->componentsAttached++;
			newComponent->onAttachment();
		}
		
		template<typename T> requires isCObjectComponent<T>
		[[nodiscard]] T* getComponent() const
		{
			constexpr std::size_t TYPE_ID = T::TYPE_ID;
			static_assert(TYPE_ID < components::COBJECT_COMPONENTS,
				"Object component type id exceeded component storage");

			if (this->components[TYPE_ID] != nullptr)
			{
				return static_cast<T*>(this->components[TYPE_ID]);
			}

			return nullptr;
		}

		template<typename T> requires isCObjectComponent<T>
		[[nodiscard]] bool hasComponent() const
		{
			constexpr std::size_t TYPE_ID = T::TYPE_ID;
			static_assert(TYPE_ID < components::COBJECT_COMPONENTS,
				"Object component type id exceeded component storage");

			if (this->components[TYPE_ID] == nullptr)
			{
				return false;
			}

			return true;
		}

		[[nodiscard]] uint32 getComponentsAttached() const;

		//
	};
}