#pragma once

#include "src/scene/Scene.h"

#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace world
{
class SceneCommandBuffer final
{
  public:
	SceneCommandBuffer() = default;
	SceneCommandBuffer(const SceneCommandBuffer &) = delete;
	SceneCommandBuffer &operator=(const SceneCommandBuffer &) = delete;

	void DestroyObject(ObjectHandle Object);

	template <IsCObjectComponent T, typename... ArgumentTypes> void AddComponent(ObjectHandle Object, ArgumentTypes &&...Arguments)
	{
		static_assert(TypeListContains<T, components::ComponentTypeList>::Value, "Component must be registered");
		using CommandType = AddComponentCommand<T, std::decay_t<ArgumentTypes>...>;
		auto Command = std::make_unique<CommandType>(Object, std::forward<ArgumentTypes>(Arguments)...);
		std::scoped_lock Lock(this->Mutex);
		this->Commands.push_back(std::move(Command));
	}

	template <IsCObjectComponent T> void RemoveComponent(ObjectHandle Object)
	{
		static_assert(TypeListContains<T, components::ComponentTypeList>::Value, "Component must be registered");
		auto Command = std::make_unique<RemoveComponentCommand<T>>(Object);
		std::scoped_lock Lock(this->Mutex);
		this->Commands.push_back(std::move(Command));
	}

	[[nodiscard]] usize Size() const noexcept;
	[[nodiscard]] bool Empty() const noexcept;
	void Execute(Scene &Scene);
	void Clear() noexcept;

  private:
	class Command
	{
	  public:
		virtual ~Command() = default;
		virtual void Execute(Scene &Scene) = 0;
		[[nodiscard]] virtual string Describe() const = 0;
	};

	class DestroyObjectCommand final : public Command
	{
	  public:
		explicit DestroyObjectCommand(ObjectHandle Object) : Object(Object)
		{
		}
		void Execute(Scene &Scene) override
		{
			Scene.DestroyObject(this->Object);
		}
		[[nodiscard]] string Describe() const override
		{
			return "destroy object slot " + std::to_string(this->Object.Slot);
		}

	  private:
		ObjectHandle Object;
	};

	template <IsCObjectComponent T, typename... ArgumentTypes> class AddComponentCommand final : public Command
	{
	  public:
		template <typename... Values>
		AddComponentCommand(ObjectHandle Object, Values &&...Arguments) : Object(Object), Arguments(std::forward<Values>(Arguments)...)
		{
		}
		void Execute(Scene &Scene) override
		{
			std::apply([this, &Scene](auto &&...Values)
					   { (void)Scene.AddComponent<T>(this->Object, std::forward<decltype(Values)>(Values)...); },
					   std::move(this->Arguments));
		}
		[[nodiscard]] string Describe() const override
		{
			return "attach " + string(T::ComponentName) + " to object slot " + std::to_string(this->Object.Slot);
		}

	  private:
		ObjectHandle Object;
		std::tuple<ArgumentTypes...> Arguments;
	};

	template <IsCObjectComponent T> class RemoveComponentCommand final : public Command
	{
	  public:
		explicit RemoveComponentCommand(ObjectHandle Object) : Object(Object)
		{
		}
		void Execute(Scene &Scene) override
		{
			Scene.RemoveComponent<T>(this->Object);
		}
		[[nodiscard]] string Describe() const override
		{
			return "remove " + string(T::ComponentName) + " from object slot " + std::to_string(this->Object.Slot);
		}

	  private:
		ObjectHandle Object;
	};

	mutable std::mutex Mutex;
	std::vector<std::unique_ptr<Command>> Commands;
};
} // namespace world
