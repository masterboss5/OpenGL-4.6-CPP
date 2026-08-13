#pragma once

#include "EditorCommand.h"
#include "Source/resource/asset/MaterialAsset.h"
#include "Source/resource/asset/MeshAsset.h"
#include "Source/scene/Scene.h"

#include <optional>
#include <span>
#include <vector>

namespace editor::commands
{
class MeshMaterialOverrideCommand final : public EditorCommand
{
  public:
	MeshMaterialOverrideCommand(world::Scene &Scene, std::span<const world::ObjectHandle> Objects,
								resource::ModelMeshInstanceID MeshInstance, resource::MaterialSlotID MaterialSlot,
								resource::AssetHandle<resource::MaterialInterfaceAsset> Material);

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] bool TryMerge(const EditorCommand &Other) override;

  private:
	struct TargetState final
	{
		world::ObjectHandle Object;
		std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>> Before;
	};

	void ApplyUniform(const resource::AssetHandle<resource::MaterialInterfaceAsset> &Material);
	void RestoreBefore();

	world::Scene *Scene = nullptr;
	std::vector<TargetState> Targets;
	resource::ModelMeshInstanceID MeshInstance = 0;
	resource::MaterialSlotID MaterialSlot = 0;
	resource::AssetHandle<resource::MaterialInterfaceAsset> After;
};
} // namespace editor::commands
