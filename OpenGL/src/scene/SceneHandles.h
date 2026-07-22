#pragma once

#include "src/concepts.h"

#include "src/types.h"

#include <limits>

namespace world
{
using SceneID = uint64;
using ObjectSlot = uint32;
using ComponentSlot = uint32;
using HandleGeneration = uint32;

inline constexpr uint32 InvalidSceneSlot = std::numeric_limits<uint32>::max();

struct ObjectHandle final
{
	SceneID Scene = 0;
	ObjectSlot Slot = InvalidSceneSlot;
	HandleGeneration Generation = 0;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Scene != 0 && this->Slot != InvalidSceneSlot && this->Generation != 0;
	}

	[[nodiscard]] bool operator==(const ObjectHandle &) const noexcept = default;
};

template <IsCObjectComponent T> struct ComponentHandle final
{
	SceneID Scene = 0;
	ComponentSlot Slot = InvalidSceneSlot;
	HandleGeneration Generation = 0;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Scene != 0 && this->Slot != InvalidSceneSlot && this->Generation != 0;
	}

	[[nodiscard]] bool operator==(const ComponentHandle &) const noexcept = default;
};
} // namespace world
