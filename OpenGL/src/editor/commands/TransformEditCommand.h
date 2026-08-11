#pragma once

#include "EditorCommand.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/scene/Scene.h"

#include <span>
#include <vector>

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

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] bool TryMerge(const EditorCommand &Other) override;

  private:
	struct Entry final
	{
		world::ObjectHandle Object;
		world::ComponentHandle<components::CObjectTransformComponent> Component;
		TransformState Before;
		TransformState After;
	};

	TransformEditCommand(world::Scene &Scene, std::vector<Entry> Entries, string Name);
	void Apply(bool UseAfter);

	world::Scene *Scene = nullptr;
	std::vector<Entry> Entries;
	string Name;
};
} // namespace editor::commands
