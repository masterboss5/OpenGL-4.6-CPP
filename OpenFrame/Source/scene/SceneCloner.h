#pragma once

#include "Scene.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <memory>
#include <unordered_map>

namespace world
{
struct ENGINE_API SceneCloneResult final
{
	std::unique_ptr<Scene> ClonedScene;
	std::unordered_map<util::UUID, ObjectHandle> ObjectsByPersistentID;

	[[nodiscard]] ObjectHandle FindObject(const util::UUID &PersistentID) const noexcept;
};

class ENGINE_API SceneCloner final
{
  public:
	[[nodiscard]] static SceneCloneResult Clone(const Scene &Source, SceneCapacitySpecification Capacity = {});
};
} // namespace world
