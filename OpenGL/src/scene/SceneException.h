#pragma once

#include "src/scene/SceneHandles.h"

#include <stdexcept>

namespace world
{
class SceneException : public std::runtime_error
{
  public:
	explicit SceneException(string Diagnostic) : std::runtime_error(std::move(Diagnostic))
	{
	}
};

class InvalidObjectHandleException final : public SceneException
{
  public:
	explicit InvalidObjectHandleException(ObjectHandle Handle)
		: SceneException("Invalid or stale object handle (scene=" + std::to_string(Handle.Scene) + ", slot=" + std::to_string(Handle.Slot) +
						 ", generation=" + std::to_string(Handle.Generation) + ")")
	{
	}
};

class InvalidComponentHandleException final : public SceneException
{
  public:
	InvalidComponentHandleException(uint32 Slot, uint32 Generation, string_view ComponentName)
		: SceneException("Invalid or stale " + string(ComponentName) + " handle (slot=" + std::to_string(Slot) +
						 ", generation=" + std::to_string(Generation) + ")")
	{
	}
};

class ComponentAlreadyAttachedException final : public SceneException
{
  public:
	ComponentAlreadyAttachedException(ObjectHandle Object, string_view ComponentName)
		: SceneException("Object slot " + std::to_string(Object.Slot) + " already has component " + string(ComponentName))
	{
	}
};

class MissingComponentDependencyException final : public SceneException
{
  public:
	MissingComponentDependencyException(ObjectHandle Object, string_view ComponentName, string_view DependencyName)
		: SceneException("Cannot attach " + string(ComponentName) + " to object slot " + std::to_string(Object.Slot) +
						 ": required component " + string(DependencyName) + " is missing")
	{
	}
};

class ComponentStillRequiredException final : public SceneException
{
  public:
	ComponentStillRequiredException(ObjectHandle Object, string_view ComponentName, string_view DependentName)
		: SceneException("Cannot remove " + string(ComponentName) + " from object slot " + std::to_string(Object.Slot) +
						 ": attached component " + string(DependentName) + " depends on it")
	{
	}
};

class SceneCapacityException final : public SceneException
{
  public:
	explicit SceneCapacityException(string Diagnostic) : SceneException(std::move(Diagnostic))
	{
	}
};

class SceneCommandExecutionException final : public SceneException
{
  public:
	SceneCommandExecutionException(usize CommandIndex, string Description)
		: SceneException("Scene command " + std::to_string(CommandIndex) + " failed: " + std::move(Description)), CommandIndex(CommandIndex)
	{
	}

	[[nodiscard]] usize GetCommandIndex() const noexcept
	{
		return this->CommandIndex;
	}

  private:
	usize CommandIndex = 0;
};
} // namespace world
