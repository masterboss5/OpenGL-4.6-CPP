#pragma once

#include "EditorCommand.h"
#include "Source/editor/reflection/ReflectionRegistry.h"
#include "Source/scene/Scene.h"

#include <span>
#include <vector>

namespace editor::commands
{
class PropertyEditCommand final : public EditorCommand
{
  public:
	[[nodiscard]] static EditorCommandPtr Create(world::Scene &Scene, world::ObjectHandle Object, uint32 ComponentType,
												 reflection::PropertyDescriptor Property, reflection::PropertyValue After,
												 resource::AssetManager *Assets = nullptr);
	[[nodiscard]] static EditorCommandPtr Create(world::Scene &Scene, std::span<const world::ObjectHandle> Objects, uint32 ComponentType,
												 reflection::PropertyDescriptor Property, reflection::PropertyValue After,
												 resource::AssetManager *Assets = nullptr);

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] bool TryMerge(const EditorCommand &Other) override;

  private:
	PropertyEditCommand(world::Scene &Scene, std::vector<world::ObjectHandle> Objects, uint32 ComponentType,
						reflection::PropertyDescriptor Property, std::vector<reflection::PropertyValue> Before,
						reflection::PropertyValue After, resource::AssetManager *Assets);

	void ApplyUniform(const reflection::PropertyValue &Value);
	void ApplyValues(std::span<const reflection::PropertyValue> Values);

	world::Scene *Scene = nullptr;
	std::vector<world::ObjectHandle> Objects;
	uint32 ComponentType = 0;
	reflection::PropertyDescriptor Property;
	std::vector<reflection::PropertyValue> Before;
	reflection::PropertyValue After;
	resource::AssetManager *Assets = nullptr;
	string Name;
};
} // namespace editor::commands
