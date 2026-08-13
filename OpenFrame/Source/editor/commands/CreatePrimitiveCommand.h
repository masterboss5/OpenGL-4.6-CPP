#pragma once

#include "Source/editor/asset/PrimitiveMeshFactory.h"
#include "Source/editor/commands/EditorCommand.h"
#include "Source/resource/asset/ModelAsset.h"
#include "Source/util/UUID.h"

#include <vector>

namespace editor::document
{
class SceneDocument;
}

namespace editor::commands
{
class CreatePrimitiveCommand final : public EditorCommand
{
  public:
	CreatePrimitiveCommand(document::SceneDocument &Document, asset::PrimitiveShape Shape,
						   resource::AssetHandle<resource::ModelAsset> Model, util::UUID Parent = {});

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] const util::UUID &GetPersistentID() const noexcept;

  private:
	document::SceneDocument *Document = nullptr;
	asset::PrimitiveShape Shape = asset::PrimitiveShape::Box;
	resource::AssetHandle<resource::ModelAsset> Model;
	util::UUID PersistentID = util::UUID::GenerateRandomUUID();
	util::UUID Parent;
	std::vector<util::UUID> PreviousSelection;
	bool Present = false;
};
} // namespace editor::commands
