#include "BehaviorCommands.h"

#include <algorithm>
#include <stdexcept>

namespace editor::commands
{
namespace
{
[[nodiscard]] components::CObjectBehaviorComponent &ResolveBehaviorComponent(world::Scene::WriteAccess &Access,
																			 const world::ObjectHandle Object)
{
	const auto Handle = Access.GetComponent<components::CObjectBehaviorComponent>(Object);
	if (!Handle.IsValid())
		throw std::out_of_range("Behavior command target has no behavior component");
	return Access.Resolve(Handle);
}
} // namespace

AddBehaviorCommand::AddBehaviorCommand(world::Scene &Scene, const world::ObjectHandle Object,
									   const runtime::behavior::BehaviorDescriptor &Descriptor)
	: Scene(&Scene), Object(Object), Behavior{.Type = Descriptor.Type,
											  .TypeName = Descriptor.Name,
											  .ModuleName = Descriptor.ModuleName,
											  .StableTypeID = Descriptor.StableTypeID,
											  .SchemaVersion = Descriptor.SchemaVersion,
											  .State = components::BehaviorExecutionState::Unresolved}
{
	if (!Object.IsValid() || !Scene.Contains(Object))
		throw std::invalid_argument("Add behavior requires a live object");
	if (Descriptor.Type == 0 || Descriptor.Name.empty())
		throw std::invalid_argument("Add behavior requires a valid registered behavior descriptor");
	for (const runtime::behavior::BehaviorPropertyDescriptor &Property : Descriptor.Properties)
		this->Behavior.Properties.emplace(Property.Name, Property.DefaultValue);
}

string_view AddBehaviorCommand::GetName() const noexcept
{
	return "Add Behavior";
}

void AddBehaviorCommand::Execute()
{
	auto Access = this->Scene->Write();
	(void)ResolveBehaviorComponent(Access, this->Object).AddBehavior(this->Behavior);
}

void AddBehaviorCommand::Undo()
{
	auto Access = this->Scene->Write();
	ResolveBehaviorComponent(Access, this->Object).RemoveBehavior(this->Behavior.InstanceID);
}

RemoveBehaviorCommand::RemoveBehaviorCommand(world::Scene &Scene, const world::ObjectHandle Object, const util::UUID InstanceID)
	: Scene(&Scene), Object(Object)
{
	if (!Object.IsValid() || !Scene.Contains(Object))
		throw std::invalid_argument("Remove behavior requires a live object");
	auto Access = Scene.Read();
	const auto Handle = Access.GetComponent<components::CObjectBehaviorComponent>(Object);
	if (!Handle.IsValid())
		throw std::out_of_range("Remove behavior target has no behavior component");
	const auto &Behaviors = Access.Resolve(Handle).GetBehaviors();
	const auto Found = std::ranges::find(Behaviors, InstanceID, &components::BehaviorInstance::InstanceID);
	if (Found == Behaviors.end())
		throw std::out_of_range("Behavior instance is not attached to this object");
	this->Index = static_cast<usize>(std::distance(Behaviors.begin(), Found));
	this->Behavior = *Found;
}

string_view RemoveBehaviorCommand::GetName() const noexcept
{
	return "Remove Behavior";
}

void RemoveBehaviorCommand::Execute()
{
	auto Access = this->Scene->Write();
	ResolveBehaviorComponent(Access, this->Object).RemoveBehavior(this->Behavior.InstanceID);
}

void RemoveBehaviorCommand::Undo()
{
	auto Access = this->Scene->Write();
	(void)ResolveBehaviorComponent(Access, this->Object).InsertBehavior(this->Index, this->Behavior);
}

EditBehaviorCommand::EditBehaviorCommand(world::Scene &Scene, const world::ObjectHandle Object, components::BehaviorInstance After)
	: Scene(&Scene), Object(Object), After(std::move(After))
{
	if (!Object.IsValid() || !Scene.Contains(Object))
		throw std::invalid_argument("Edit behavior requires a live object");
	auto Access = Scene.Read();
	const auto Handle = Access.GetComponent<components::CObjectBehaviorComponent>(Object);
	if (!Handle.IsValid())
		throw std::out_of_range("Edit behavior target has no behavior component");
	const std::optional<components::BehaviorInstance> Existing = Access.Resolve(Handle).FindBehavior(this->After.InstanceID);
	if (!Existing.has_value())
		throw std::out_of_range("Behavior instance is not attached to this object");
	this->Before = *Existing;
}

string_view EditBehaviorCommand::GetName() const noexcept
{
	return "Edit Behavior";
}

void EditBehaviorCommand::Execute()
{
	this->Apply(this->After);
}

void EditBehaviorCommand::Undo()
{
	this->Apply(this->Before);
}

bool EditBehaviorCommand::TryMerge(const EditorCommand &Other)
{
	const auto *Typed = dynamic_cast<const EditBehaviorCommand *>(&Other);
	if (Typed == nullptr || Typed->Scene != this->Scene || Typed->Object != this->Object ||
		Typed->After.InstanceID != this->After.InstanceID)
		return false;
	this->After = Typed->After;
	return true;
}

void EditBehaviorCommand::Apply(const components::BehaviorInstance &Value)
{
	auto Access = this->Scene->Write();
	ResolveBehaviorComponent(Access, this->Object).ReplaceBehavior(Value);
}
} // namespace editor::commands
