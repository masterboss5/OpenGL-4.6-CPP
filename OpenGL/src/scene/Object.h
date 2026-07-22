#pragma once

#include "src/concepts.h"

#include "src/component/Components.h"
#include "src/component/object/CObjectComponent.h"
#include "src/scene/SceneHandles.h"
#include "src/types.h"

#include <array>

namespace world
{
class Scene;
namespace detail
{
template <typename T> class DenseGenerationalPool;
}

class Object final
{
	friend class Scene;
	friend class detail::DenseGenerationalPool<Object>;

  public:
	Object(const Object &) = delete;
	Object &operator=(const Object &) = delete;
	Object(Object &&) noexcept = default;
	Object &operator=(Object &&) noexcept = default;
	~Object() noexcept = default;

	[[nodiscard]] ObjectHandle GetHandle() const noexcept
	{
		return this->Self;
	}
	[[nodiscard]] uint32 GetComponentsAttached() const noexcept
	{
		return this->ComponentsAttached;
	}

	template <IsCObjectComponent T> [[nodiscard]] bool HasComponent() const noexcept
	{
		static_assert(T::TypeID < components::CObjectComponents);
		return this->Components[T::TypeID] != nullptr;
	}

  private:
	ObjectHandle Self;
	uint32 ComponentsAttached = 0;
	std::array<components::CObjectComponent *, components::CObjectComponents> Components{};

	explicit Object(ObjectHandle Self) noexcept : Self(Self)
	{
	}
};
} // namespace world
