#include "TransformEditCommand.h"

#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/editor/document/SceneDocument.h"

#include <stdexcept>
#include <utility>

namespace editor::commands
{
EditorCommandPtr TransformEditCommand::Create(world::Scene &Scene, const std::span<const TransformEditTarget> Targets, string Name)
{
	if (Targets.empty())
		throw std::invalid_argument("A transform edit requires at least one target");

	std::vector<Entry> Entries;
	Entries.reserve(Targets.size());
	for (const TransformEditTarget &Target : Targets)
	{
		if (!Target.Object.IsValid())
			throw std::invalid_argument("A transform edit cannot target an invalid object handle");
		const world::ComponentHandle<components::CObjectTransformComponent> Component =
			Scene.GetComponent<components::CObjectTransformComponent>(Target.Object);
		if (!Component.IsValid())
			throw world::SceneException("Transform edit target has no CObjectTransformComponent");
		const world::ComponentHandle<components::CObjectIdentityComponent> Identity =
			Scene.GetComponent<components::CObjectIdentityComponent>(Target.Object);
		if (Identity.IsValid())
		{
			auto Access = Scene.Read();
			if (Access.Resolve(Identity).IsLocked())
				throw world::SceneException("Transform edit target is locked");
		}
		Entries.push_back({.Object = Target.Object, .Component = Component, .Before = Target.Before, .After = Target.After});
	}
	return EditorCommandPtr(new TransformEditCommand(Scene, nullptr, std::move(Entries), std::move(Name)));
}

EditorCommandPtr TransformEditCommand::Create(document::SceneDocument &Document, const std::span<const TransformEditTarget> Targets,
											 string Name)
{
	EditorCommandPtr Command = Create(Document.GetScene(), Targets, std::move(Name));
	auto *TransformCommand = static_cast<TransformEditCommand *>(Command.get());
	TransformCommand->Document = &Document;
	const auto Access = Document.GetScene().Read();
	for (Entry &Entry : TransformCommand->Entries)
	{
		const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Entry.Object);
		if (!Identity.IsValid())
			continue;
		const util::UUID ID = Access.Resolve(Identity).GetPersistentID();
		if (Document.GetInstances().Contains(ID))
			Entry.InstanceID = ID;
	}
	return Command;
}

TransformEditCommand::TransformEditCommand(world::Scene &Scene, document::SceneDocument *Document, std::vector<Entry> Entries, string Name)
	: Scene(&Scene), Document(Document), Entries(std::move(Entries)), Name(std::move(Name))
{
	if (this->Name.empty())
		throw std::invalid_argument("A transform edit command requires a name");
}

string_view TransformEditCommand::GetName() const noexcept
{
	return this->Name;
}

void TransformEditCommand::Execute()
{
	this->Apply(true);
}

void TransformEditCommand::Undo()
{
	this->Apply(false);
}

bool TransformEditCommand::TryMerge(const EditorCommand &Other)
{
	const auto *TransformOther = dynamic_cast<const TransformEditCommand *>(&Other);
	if (TransformOther == nullptr || TransformOther->Scene != this->Scene || TransformOther->Document != this->Document ||
		TransformOther->Entries.size() != this->Entries.size() ||
		TransformOther->Name != this->Name)
	{
		return false;
	}
	for (usize Index = 0; Index < this->Entries.size(); ++Index)
	{
		if (this->Entries[Index].Object != TransformOther->Entries[Index].Object ||
			this->Entries[Index].InstanceID != TransformOther->Entries[Index].InstanceID ||
			this->Entries[Index].Component != TransformOther->Entries[Index].Component)
		{
			return false;
		}
	}
	for (usize Index = 0; Index < this->Entries.size(); ++Index)
		this->Entries[Index].After = TransformOther->Entries[Index].After;
	return true;
}

void TransformEditCommand::Apply(const bool UseAfter)
{
	if (this->Document != nullptr)
	{
		for (const Entry &Entry : this->Entries)
		{
			if (!Entry.InstanceID.IsValid())
				continue;
			const TransformState &State = UseAfter ? Entry.After : Entry.Before;
			this->Document->SetInstanceWorldTransform(Entry.InstanceID, State.Position, State.Rotation, State.Scale);
		}
	}
	auto Access = this->Scene->Write();
	for (const Entry &Entry : this->Entries)
	{
		if (!Entry.InstanceID.IsValid())
			(void)Access.Resolve(Entry.Component);
	}
	for (const Entry &Entry : this->Entries)
	{
		if (Entry.InstanceID.IsValid())
			continue;
		components::CObjectTransformComponent &Component = Access.Resolve(Entry.Component);
		const TransformState &State = UseAfter ? Entry.After : Entry.Before;
		Component.SetTransform(State.Position, State.Rotation, State.Scale);
	}
}
} // namespace editor::commands
