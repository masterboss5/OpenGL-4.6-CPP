#include "InstanceCommands.h"

#include "Source/editor/document/SceneDocument.h"

#include <array>
#include <stdexcept>
#include <unordered_set>

namespace editor::commands
{
InstanceArchive CaptureInstanceArchive(const instance::InstanceGraph &Graph, const std::span<const util::UUID> Selection)
{
	if (Selection.empty())
		throw std::invalid_argument("Capturing instances requires a non-empty selection");
	const instance::InstanceGraphSnapshot Snapshot = Graph.Snapshot();
	std::unordered_map<util::UUID, const instance::InstanceRecord *> ByID;
	ByID.reserve(Snapshot.Instances.size());
	for (const instance::InstanceRecord &Record : Snapshot.Instances)
		ByID.emplace(Record.ID, &Record);
	std::unordered_set<util::UUID> Selected;
	Selected.reserve(Selection.size());
	for (const util::UUID &ID : Selection)
	{
		const auto Found = ByID.find(ID);
		if (Found == ByID.end())
			throw std::out_of_range("Selected instance no longer exists");
		if (Found->second->Protected)
			throw std::logic_error("Protected service instances cannot be copied");
		Selected.emplace(ID);
	}
	InstanceArchive Result;
	for (const util::UUID &ID : Selection)
	{
		const instance::InstanceRecord *Current = ByID.at(ID);
		bool SelectedAncestor = false;
		while (Current->Parent.IsValid())
		{
			if (Selected.contains(Current->Parent))
			{
				SelectedAncestor = true;
				break;
			}
			Current = ByID.at(Current->Parent);
		}
		if (!SelectedAncestor)
			Result.Roots.push_back(ID);
	}
	std::unordered_set<util::UUID> Included;
	Included.reserve(Snapshot.Instances.size());
	for (const util::UUID &Root : Result.Roots)
	{
		std::vector<util::UUID> Pending{Root};
		while (!Pending.empty())
		{
			const util::UUID Current = Pending.back();
			Pending.pop_back();
			if (!Included.emplace(Current).second)
				continue;
			const instance::InstanceRecord &Record = *ByID.at(Current);
			Result.Records.push_back(Record);
			for (auto Child = Record.Children.rbegin(); Child != Record.Children.rend(); ++Child)
				Pending.push_back(*Child);
		}
	}
	if (Result.Empty() || Result.Records.empty())
		throw std::logic_error("Selected instances did not resolve to an archive");
	return Result;
}

CreateInstanceCommand::CreateInstanceCommand(document::SceneDocument &Document, const instance::InstanceClassID ClassID,
											 const util::UUID Parent, instance::InstancePropertyMap InitialProperties)
	: Document(&Document), ClassID(ClassID), Parent(Parent), InitialProperties(std::move(InitialProperties)),
	  PreviousSelection(Document.GetSelection().GetOrdered())
{
	if (Document.GetInstanceTypes().Find(ClassID) == nullptr)
		throw std::invalid_argument("CreateInstanceCommand class is not registered");
	if (!Parent.IsValid() || !Document.GetInstances().Contains(Parent))
		throw std::out_of_range("CreateInstanceCommand parent does not exist");
}

string_view CreateInstanceCommand::GetName() const noexcept
{
	return "Create Instance";
}

void CreateInstanceCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("CreateInstanceCommand is already present");
	(void)this->Document->CreateInstance(this->ClassID, this->Parent, {}, this->ID, this->InitialProperties);
	this->Present = true;
}

void CreateInstanceCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("CreateInstanceCommand is not present");
	this->Document->DestroyInstance(this->ID);
	this->Document->GetSelection().Clear();
	for (const util::UUID &Previous : this->PreviousSelection)
	{
		if (this->Document->GetInstances().Contains(Previous))
			this->Document->GetSelection().Add(Previous);
	}
	this->Present = false;
}

const util::UUID &CreateInstanceCommand::GetInstanceID() const noexcept
{
	return this->ID;
}

RenameInstanceCommand::RenameInstanceCommand(document::SceneDocument &Document, const util::UUID ID, string Name)
	: Document(&Document), ID(ID), Before(Document.GetInstances().Get(ID).Name), After(std::move(Name))
{
	if (this->After.empty())
		throw std::invalid_argument("RenameInstanceCommand name cannot be empty");
}

string_view RenameInstanceCommand::GetName() const noexcept
{
	return "Rename Instance";
}

void RenameInstanceCommand::Execute()
{
	if (this->Applied)
		throw std::logic_error("RenameInstanceCommand is already applied");
	this->Document->RenameInstance(this->ID, this->After);
	this->Applied = true;
}

void RenameInstanceCommand::Undo()
{
	if (!this->Applied)
		throw std::logic_error("RenameInstanceCommand is not applied");
	this->Document->RenameInstance(this->ID, this->Before);
	this->Applied = false;
}

ReparentInstanceCommand::ReparentInstanceCommand(document::SceneDocument &Document, const util::UUID ID, const util::UUID Parent,
												 const uint32 SiblingOrder)
	: Document(&Document), ID(ID), AfterParent(Parent), AfterOrder(SiblingOrder)
{
	const instance::InstanceRecord Record = Document.GetInstances().Get(ID);
	this->BeforeParent = Record.Parent;
	this->BeforeOrder = Record.SiblingOrder;
}

string_view ReparentInstanceCommand::GetName() const noexcept
{
	return "Reparent Instance";
}

void ReparentInstanceCommand::Execute()
{
	if (this->Applied)
		throw std::logic_error("ReparentInstanceCommand is already applied");
	this->Document->ReparentInstance(this->ID, this->AfterParent, this->AfterOrder);
	this->Applied = true;
}

void ReparentInstanceCommand::Undo()
{
	if (!this->Applied)
		throw std::logic_error("ReparentInstanceCommand is not applied");
	this->Document->ReparentInstance(this->ID, this->BeforeParent, this->BeforeOrder);
	this->Applied = false;
}

SetInstancePropertyCommand::SetInstancePropertyCommand(document::SceneDocument &Document, const util::UUID ID, string Name,
													   instance::InstancePropertyValue Value)
	: Document(&Document), ID(ID), Name(std::move(Name)), After(std::move(Value))
{
	const instance::InstanceRecord Record = Document.GetInstances().Get(ID);
	const auto Existing = Record.Properties.find(this->Name);
	if (Existing != Record.Properties.end())
		this->Before = Existing->second;
}

string_view SetInstancePropertyCommand::GetName() const noexcept
{
	return "Set Instance Property";
}

void SetInstancePropertyCommand::Execute()
{
	if (this->Applied)
		throw std::logic_error("SetInstancePropertyCommand is already applied");
	this->Document->SetInstanceProperty(this->ID, this->Name, this->After);
	this->Applied = true;
}

void SetInstancePropertyCommand::Undo()
{
	if (!this->Applied)
		throw std::logic_error("SetInstancePropertyCommand is not applied");
	if (this->Before.has_value())
		this->Document->SetInstanceProperty(this->ID, this->Name, *this->Before);
	else
		this->Document->RemoveInstanceProperty(this->ID, this->Name);
	this->Applied = false;
}

bool SetInstancePropertyCommand::TryMerge(const EditorCommand &Other)
{
	const auto *Replacement = dynamic_cast<const SetInstancePropertyCommand *>(&Other);
	if (Replacement == nullptr || Replacement->Document != this->Document || Replacement->ID != this->ID ||
		Replacement->Name != this->Name || !Replacement->Applied)
	{
		return false;
	}
	this->After = Replacement->After;
	return true;
}

RemoveInstancePropertyCommand::RemoveInstancePropertyCommand(document::SceneDocument &Document, const util::UUID ID, string Name)
	: Document(&Document), ID(ID), Name(std::move(Name))
{
	const instance::InstanceRecord Record = Document.GetInstances().Get(ID);
	const auto Property = Record.Properties.find(this->Name);
	if (Property == Record.Properties.end())
		throw std::out_of_range("Instance property does not exist");
	this->Before = Property->second;
}

string_view RemoveInstancePropertyCommand::GetName() const noexcept
{
	return "Remove Instance Property";
}

void RemoveInstancePropertyCommand::Execute()
{
	if (this->Removed)
		throw std::logic_error("RemoveInstancePropertyCommand is already applied");
	this->Document->RemoveInstanceProperty(this->ID, this->Name);
	this->Removed = true;
}

void RemoveInstancePropertyCommand::Undo()
{
	if (!this->Removed)
		throw std::logic_error("RemoveInstancePropertyCommand is not applied");
	this->Document->SetInstanceProperty(this->ID, this->Name, this->Before);
	this->Removed = false;
}

namespace
{
[[nodiscard]] std::vector<instance::InstanceRecord> CaptureSubtree(const instance::InstanceGraph &Graph, const util::UUID Root)
{
	const std::array Selection{Root};
	return CaptureInstanceArchive(Graph, Selection).Records;
}

void RestoreArchive(document::SceneDocument &Document, const std::vector<instance::InstanceRecord> &Archive,
					const std::unordered_map<util::UUID, util::UUID> *Remap = nullptr, const util::UUID &ParentOverride = {},
					std::vector<util::UUID> *CreatedRoots = nullptr)
{
	instance::InstanceGraph &Graph = Document.GetInstances();
	std::unordered_set<util::UUID> ArchiveIDs;
	ArchiveIDs.reserve(Archive.size());
	for (const instance::InstanceRecord &Record : Archive)
		ArchiveIDs.emplace(Record.ID);
	std::vector<util::UUID> LocalRoots;
	try
	{
		for (const instance::InstanceRecord &Source : Archive)
		{
			const util::UUID ID = Remap == nullptr ? Source.ID : Remap->at(Source.ID);
			const bool Root = !ArchiveIDs.contains(Source.Parent);
			const util::UUID Parent = Root && ParentOverride.IsValid()
										  ? ParentOverride
										  : (Remap != nullptr && Remap->contains(Source.Parent) ? Remap->at(Source.Parent) : Source.Parent);
			(void)Document.CreateInstance(Source.ClassID, Parent, Source.Name, ID);
			if (Root)
				LocalRoots.push_back(ID);
			Document.ReparentInstance(ID, Parent, Source.SiblingOrder);
			Graph.SetEnabled(ID, Source.Enabled);
			for (const auto &[Name, OriginalValue] : Source.Properties)
			{
				instance::InstancePropertyValue Value = OriginalValue;
				if (Remap != nullptr)
				{
					if (auto *Reference = std::get_if<util::UUID>(&Value); Reference != nullptr && Remap->contains(*Reference))
						*Reference = Remap->at(*Reference);
				}
				Document.SetInstanceProperty(ID, Name, std::move(Value));
			}
		}
	}
	catch (...)
	{
		for (auto Root = LocalRoots.rbegin(); Root != LocalRoots.rend(); ++Root)
			if (Graph.Contains(*Root))
				Document.DestroyInstance(*Root);
		throw;
	}
	if (CreatedRoots != nullptr)
		*CreatedRoots = std::move(LocalRoots);
}
} // namespace

DeleteInstanceCommand::DeleteInstanceCommand(document::SceneDocument &Document, const util::UUID ID)
	: Document(&Document), ID(ID), Archive(CaptureSubtree(Document.GetInstances(), ID)),
	  PreviousSelection(Document.GetSelection().GetOrdered())
{
	if (this->Archive.front().Protected)
		throw std::logic_error("Protected service instances cannot be deleted");
}

string_view DeleteInstanceCommand::GetName() const noexcept
{
	return "Delete Instance";
}

void DeleteInstanceCommand::Execute()
{
	if (!this->Present)
		throw std::logic_error("DeleteInstanceCommand instance is not present");
	this->Document->DestroyInstance(this->ID);
	this->Document->GetSelection().Prune(this->Document->GetInstances());
	this->Present = false;
}

void DeleteInstanceCommand::Undo()
{
	if (this->Present)
		throw std::logic_error("DeleteInstanceCommand instance is already present");
	RestoreArchive(*this->Document, this->Archive);
	this->Document->GetSelection().Clear();
	for (const util::UUID &Previous : this->PreviousSelection)
	{
		if (this->Document->GetInstances().Contains(Previous))
			this->Document->GetSelection().Add(Previous);
	}
	this->Present = true;
}

DuplicateInstanceCommand::DuplicateInstanceCommand(document::SceneDocument &Document, const util::UUID Source)
	: Document(&Document), Source(Source), SourceArchive(CaptureSubtree(Document.GetInstances(), Source)),
	  PreviousSelection(Document.GetSelection().GetOrdered())
{
	if (this->SourceArchive.front().Protected)
		throw std::logic_error("Protected service instances cannot be duplicated");
	for (const instance::InstanceRecord &Record : this->SourceArchive)
		this->Remap.emplace(Record.ID, util::UUID::GenerateRandomUUID());
	this->DuplicateID = this->Remap.at(Source);
}

string_view DuplicateInstanceCommand::GetName() const noexcept
{
	return "Duplicate Instance";
}

void DuplicateInstanceCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("DuplicateInstanceCommand duplicate is already present");
	RestoreArchive(*this->Document, this->SourceArchive, &this->Remap);
	this->Document->GetSelection().SelectOnly(this->DuplicateID);
	this->Present = true;
}

void DuplicateInstanceCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("DuplicateInstanceCommand duplicate is not present");
	this->Document->DestroyInstance(this->DuplicateID);
	this->Document->GetSelection().Clear();
	for (const util::UUID &Previous : this->PreviousSelection)
	{
		if (this->Document->GetInstances().Contains(Previous))
			this->Document->GetSelection().Add(Previous);
	}
	this->Present = false;
}

const util::UUID &DuplicateInstanceCommand::GetDuplicateID() const noexcept
{
	return this->DuplicateID;
}

PasteInstanceArchiveCommand::PasteInstanceArchiveCommand(document::SceneDocument &Document, InstanceArchive Archive,
														 const util::UUID ParentOverride)
	: Document(&Document), Archive(std::move(Archive)), ParentOverride(ParentOverride),
	  PreviousSelection(Document.GetSelection().GetOrdered())
{
	if (this->Archive.Empty() || this->Archive.Records.empty())
		throw std::invalid_argument("PasteInstanceArchiveCommand requires a non-empty archive");
	if (this->ParentOverride.IsValid() && !Document.GetInstances().Contains(this->ParentOverride))
		throw std::out_of_range("PasteInstanceArchiveCommand parent does not exist");
	std::unordered_set<util::UUID> IDs;
	IDs.reserve(this->Archive.Records.size());
	for (const instance::InstanceRecord &Record : this->Archive.Records)
	{
		if (!IDs.emplace(Record.ID).second)
			throw std::invalid_argument("PasteInstanceArchiveCommand archive contains duplicate identities");
		this->Remap.emplace(Record.ID, util::UUID::GenerateRandomUUID());
	}
	for (const util::UUID &Root : this->Archive.Roots)
		if (!IDs.contains(Root))
			throw std::invalid_argument("PasteInstanceArchiveCommand archive root is missing");
}

string_view PasteInstanceArchiveCommand::GetName() const noexcept
{
	return "Paste Instances";
}

void PasteInstanceArchiveCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("PasteInstanceArchiveCommand instances are already present");
	if (this->ParentOverride.IsValid() && !this->Document->GetInstances().Contains(this->ParentOverride))
		throw std::out_of_range("PasteInstanceArchiveCommand parent no longer exists");
	RestoreArchive(*this->Document, this->Archive.Records, &this->Remap, this->ParentOverride, &this->CreatedRoots);
	this->Document->GetSelection().Clear();
	for (const util::UUID &Root : this->Archive.Roots)
		this->Document->GetSelection().Add(this->Remap.at(Root));
	this->Present = true;
}

void PasteInstanceArchiveCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("PasteInstanceArchiveCommand instances are not present");
	for (auto Root = this->CreatedRoots.rbegin(); Root != this->CreatedRoots.rend(); ++Root)
		if (this->Document->GetInstances().Contains(*Root))
			this->Document->DestroyInstance(*Root);
	this->Document->GetSelection().Clear();
	for (const util::UUID &Previous : this->PreviousSelection)
		if (this->Document->GetInstances().Contains(Previous))
			this->Document->GetSelection().Add(Previous);
	this->Present = false;
}
} // namespace editor::commands
