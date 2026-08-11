#pragma once

#include "src/component/object/CObjectIdentityComponent.h"
#include "src/core/threading/TaskScheduler.h"
#include "src/scene/Scene.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <future>
#include <vector>

namespace editor::hierarchy
{
inline constexpr uint32 InvalidHierarchyRow = ~uint32{0};

struct SceneHierarchyRow final
{
	util::UUID PersistentID;
	world::ObjectHandle Object;
	string Name;
	std::vector<string> Tags;
	uint32 Depth = 0;
	uint32 ParentRow = InvalidHierarchyRow;
	uint32 ChildCount = 0;
	uint32 SiblingOrder = 0;
	components::ObjectMobility Mobility = components::ObjectMobility::Movable;
	bool Enabled = true;
	bool EditorVisible = true;
	bool Locked = false;
};

struct SceneHierarchySnapshot final
{
	uint64 SceneRevision = 0;
	std::vector<SceneHierarchyRow> Rows;
};

class SceneHierarchyBuilder final
{
  public:
	[[nodiscard]] static SceneHierarchySnapshot Build(const world::Scene &Scene, uint64 SceneRevision);
	[[nodiscard]] static std::future<SceneHierarchySnapshot> BuildAsync(core::threading::TaskScheduler &Scheduler,
																		const world::Scene &Scene, uint64 SceneRevision);
	[[nodiscard]] static SceneHierarchySnapshot Filter(const SceneHierarchySnapshot &Source, string_view Query);
};
} // namespace editor::hierarchy
