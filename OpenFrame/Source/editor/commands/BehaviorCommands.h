#pragma once

#include "EditorCommand.h"
#include "Source/component/object/CObjectBehaviorComponent.h"
#include "Source/runtime/behavior/BehaviorRegistry.h"
#include "Source/scene/Scene.h"

namespace editor::commands
{
class AddBehaviorCommand final : public EditorCommand
{
  public:
	AddBehaviorCommand(world::Scene &Scene, world::ObjectHandle Object, const runtime::behavior::BehaviorDescriptor &Descriptor);

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	world::Scene *Scene = nullptr;
	world::ObjectHandle Object;
	components::BehaviorInstance Behavior;
};

class RemoveBehaviorCommand final : public EditorCommand
{
  public:
	RemoveBehaviorCommand(world::Scene &Scene, world::ObjectHandle Object, util::UUID InstanceID);

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;

  private:
	world::Scene *Scene = nullptr;
	world::ObjectHandle Object;
	components::BehaviorInstance Behavior;
	usize Index = 0;
};

class EditBehaviorCommand final : public EditorCommand
{
  public:
	EditBehaviorCommand(world::Scene &Scene, world::ObjectHandle Object, components::BehaviorInstance After);

	[[nodiscard]] string_view GetName() const noexcept override;
	void Execute() override;
	void Undo() override;
	[[nodiscard]] bool TryMerge(const EditorCommand &Other) override;

  private:
	void Apply(const components::BehaviorInstance &Value);

	world::Scene *Scene = nullptr;
	world::ObjectHandle Object;
	components::BehaviorInstance Before;
	components::BehaviorInstance After;
};
} // namespace editor::commands
