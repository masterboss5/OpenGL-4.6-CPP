#pragma once

#include "Source/core/threading/TaskScheduler.h"
#include "Source/resource/asset/MaterialAsset.h"
#include "Source/resource/asset/MeshAsset.h"
#include "Source/resource/asset/ModelAsset.h"
#include "Source/scene/SceneHandles.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace editor::asset
{
class AssetRegistry;
}

namespace editor::document
{
class SceneDocument;
}

namespace resource
{
class AssetManager;
}

namespace editor::material
{
struct PrivateMaterialTarget final
{
	world::ObjectHandle Object;
	resource::ModelMeshInstanceID MeshInstance = 0;
	resource::MaterialSlotID MaterialSlot = 0;
};

struct PrivateMaterialAssignmentResult final
{
	util::UUID OperationID;
	bool Committed = false;
	string Diagnostic;
};

class PrivateMaterialAssignmentService final
{
  public:
	PrivateMaterialAssignmentService(resource::AssetManager &Assets, std::filesystem::path ContentRoot,
									 std::filesystem::path IntermediateRoot, std::filesystem::path TrashRoot);
	~PrivateMaterialAssignmentService();

	PrivateMaterialAssignmentService(const PrivateMaterialAssignmentService &) = delete;
	PrivateMaterialAssignmentService &operator=(const PrivateMaterialAssignmentService &) = delete;
	PrivateMaterialAssignmentService(PrivateMaterialAssignmentService &&) = delete;
	PrivateMaterialAssignmentService &operator=(PrivateMaterialAssignmentService &&) = delete;

	[[nodiscard]] util::UUID BeginBaseColorAssignment(document::SceneDocument &Document, std::span<const PrivateMaterialTarget> Targets,
													  const glm::vec4 &BaseColor, core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] util::UUID BeginBaseColorPreview(document::SceneDocument &Document, std::span<const PrivateMaterialTarget> Targets,
												   const glm::vec4 &BaseColor);
	void UpdateBaseColorPreview(const util::UUID &PreviewID, const glm::vec4 &BaseColor);
	[[nodiscard]] util::UUID CommitBaseColorPreview(const util::UUID &PreviewID, core::threading::TaskScheduler &Scheduler);
	void CancelBaseColorPreview(const util::UUID &PreviewID);
	[[nodiscard]] std::vector<util::UUID> ClonePrivateAssignments(document::SceneDocument &Document,
																  std::span<const world::ObjectHandle> Objects,
																  core::threading::TaskScheduler &Scheduler);
	void QueueRetirementCandidates(std::vector<resource::AssetID> Assets, core::threading::TaskScheduler &Scheduler) noexcept;
	[[nodiscard]] bool Poll(core::threading::TaskScheduler &Scheduler, asset::AssetRegistry &Registry);
	[[nodiscard]] bool Cancel(const util::UUID &OperationID, core::threading::TaskScheduler &Scheduler);
	void Wait() noexcept;
	void Shutdown() noexcept;

	[[nodiscard]] bool HasPendingWork() const;
	[[nodiscard]] bool IsShutdown() const noexcept;
	[[nodiscard]] std::optional<PrivateMaterialAssignmentResult> TakeResult(const util::UUID &OperationID);
	[[nodiscard]] std::vector<PrivateMaterialAssignmentResult> TakeResults();

  private:
	class Implementation;
	std::unique_ptr<Implementation> State;
};
} // namespace editor::material
