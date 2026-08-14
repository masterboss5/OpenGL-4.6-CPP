#pragma once

#include "EditorCommand.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/scene/Scene.h"

#include <span>
#include <vector>

namespace editor::document
{
class SceneDocument;
}

namespace editor::commands
{
struct TransformState final
{
	glm::vec3 Position{0.0f};
	glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 Scale{1.0f};

	[[nodiscard]] bool operator==(const TransformState &) const noexcept = default;
};

struct TransformEditTarget final
{
	world::ObjectHandle Object;
	TransformState Before;
	TransformState After;
};

class TransformEditCommand final : public EditorCommand
{
  public:
	[[nodiscard]] static EditorCommandPtr Create(world::Scene &Scene, std::span<const TransformEditTarget> Targets,
												 string Name = "Transform selection");
	[[nodiscard]] static EditorCommandPtr Create(document::SceneDocument &Document, std::span<const TransformEditTarget> Targets,
												 string Name = "Transform selection");

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] bool TryMerge(const EditorCommand &Other) override;

  private:
	struct Entry final
	{
		world::ObjectHandle Object;
		world::ComponentHandle<components::CObjectTransformComponent> Component;
		util::UUID InstanceID;
		TransformState Before;
		TransformState After;
	};

	TransformEditCommand(world::Scene &Scene, document::SceneDocument *Document, std::vector<Entry> Entries, string Name);
	void Apply(bool UseAfter);

	world::Scene *Scene = nullptr;
	document::SceneDocument *Document = nullptr;
	std::vector<Entry> Entries;
	string Name;
};
} // namespace editor::commands
