#pragma once

#include "Source/concepts.h"

#include "Source/types.h"

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

struct ObjectHandleHash final
{
	[[nodiscard]] usize operator()(const ObjectHandle &Handle) const noexcept
	{
		uint64 Hash = 1'469'598'103'934'665'603ULL;
		Hash = (Hash ^ Handle.Scene) * 1'099'511'628'211ULL;
		Hash = (Hash ^ Handle.Slot) * 1'099'511'628'211ULL;
		Hash = (Hash ^ Handle.Generation) * 1'099'511'628'211ULL;
		return static_cast<usize>(Hash);
	}
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
