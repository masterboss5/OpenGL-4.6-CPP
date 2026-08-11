#include "SceneDocument.h"

#include "src/component/object/CObjectHierarchyComponent.h"
#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectTransformComponent.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace editor::document
{
namespace
{
[[nodiscard]] uint64 AdvanceRevision(const uint64 Revision, uint64 Delta) noexcept
{
	if (Delta == 0)
		return Revision;
	const uint64 RemainingBeforeRollover = std::numeric_limits<uint64>::max() - Revision;
	if (Delta <= RemainingBeforeRollover)
		return Revision + Delta;
	Delta -= RemainingBeforeRollover + 1U;
	return 1U + Delta % std::numeric_limits<uint64>::max();
}

[[nodiscard]] uint64 RevisionDistance(const uint64 Baseline, const uint64 Current) noexcept
{
	if (Current >= Baseline)
		return Current - Baseline;
	return (std::numeric_limits<uint64>::max() - Baseline) + Current;
}
} // namespace

SceneDocument::SceneDocument(string Name, const world::SceneCapacitySpecification Capacity, const util::UUID ID,
							 const usize CommandHistoryCapacity)
	: ID(ID), Name(std::move(Name)), Scene(std::make_unique<world::Scene>(Capacity)),
	  History(
		  CommandHistoryCapacity,
		  [this]()
		  {
			  this->Selection.Prune(*this->Scene);
			  this->MarkModified();
		  },
		  [this]() noexcept { return this->GetRevision(); }),
	  OwnerThread(std::this_thread::get_id())
{
	if (this->Name.empty())
		throw std::invalid_argument("Scene document name cannot be empty");
	if (!this->ID.IsValid())
		throw std::invalid_argument("Scene document requires a valid persistent identity");
	this->RevisionSceneBaseline = this->Scene->GetMutationRevision();
}

world::ObjectHandle SceneDocument::CreateObject(string Name, const world::ObjectHandle Parent, const util::UUID PersistentID)
{
	return this->CreateObject(SceneObjectSpecification{.Name = std::move(Name), .Parent = Parent, .PersistentID = PersistentID});
}

world::ObjectHandle SceneDocument::CreateObject(SceneObjectSpecification Specification)
{
	this->AssertOwnerThread();
	if (Specification.Name.empty())
		throw std::invalid_argument("Scene object name cannot be empty");
	if (!Specification.PersistentID.IsValid())
		throw std::invalid_argument("Scene object requires a valid persistent identity");
	if (this->Scene->FindObject(Specification.PersistentID).IsValid())
		throw std::invalid_argument("Scene object persistent identity is already present");
	if (Specification.Parent.IsValid() && !this->Scene->Contains(Specification.Parent))
		throw world::InvalidObjectHandleException(Specification.Parent);

	const world::ObjectHandle Object = this->Scene->CreateObject();
	try
	{
		(void)this->Scene->AddComponent<components::CObjectIdentityComponent>(Object, std::move(Specification.Name),
																			  Specification.PersistentID);
		(void)this->Scene->AddComponent<components::CObjectTransformComponent>(Object);
		(void)this->Scene->AddComponent<components::CObjectHierarchyComponent>(Object);
		if (Specification.Parent.IsValid())
			this->Scene->SetParent(Object, Specification.Parent);
	}
	catch (...)
	{
		this->Scene->DestroyObject(Object);
		throw;
	}

	const auto Identity = this->Scene->GetComponent<components::CObjectIdentityComponent>(Object);
	{
		auto Access = this->Scene->Read();
		this->Selection.SelectOnly(Access.Resolve(Identity).GetPersistentID());
	}
	this->MarkModified();
	return Object;
}

void SceneDocument::DestroyObject(const util::UUID &PersistentID)
{
	this->AssertOwnerThread();
	const world::ObjectHandle Object = this->Scene->FindObject(PersistentID);
	if (!Object.IsValid())
		throw std::out_of_range("Cannot destroy an object identity that is not present in the scene document");
	this->Selection.Remove(PersistentID);
	this->Scene->DestroyObject(Object);
	this->Selection.Prune(*this->Scene);
	this->MarkModified();
}

void SceneDocument::SetParent(const util::UUID &Object, const util::UUID &Parent, const uint32 SiblingOrder)
{
	this->AssertOwnerThread();
	const world::ObjectHandle ObjectHandle = this->Scene->FindObject(Object);
	if (!ObjectHandle.IsValid())
		throw std::out_of_range("Cannot parent an object identity that is not present in the scene document");
	const world::ObjectHandle ParentHandle = Parent.IsValid() ? this->Scene->FindObject(Parent) : world::ObjectHandle{};
	if (Parent.IsValid() && !ParentHandle.IsValid())
		throw std::out_of_range("Cannot use a parent identity that is not present in the scene document");
	this->Scene->SetParent(ObjectHandle, ParentHandle, SiblingOrder);
	this->MarkModified();
}

void SceneDocument::Execute(commands::EditorCommandPtr Command)
{
	this->AssertOwnerThread();
	this->History.Execute(std::move(Command));
}

void SceneDocument::Undo()
{
	this->AssertOwnerThread();
	if (!this->History.CanUndo())
		return;
	this->History.Undo();
}

void SceneDocument::Redo()
{
	this->AssertOwnerThread();
	if (!this->History.CanRedo())
		return;
	this->History.Redo();
}

void SceneDocument::MarkSaved(std::filesystem::path Path)
{
	this->AssertOwnerThread();
	if (Path.empty())
		throw std::invalid_argument("Saved scene path cannot be empty");
	this->Path = std::move(Path);
	this->SavedRevision = this->GetRevision();
}

void SceneDocument::MarkRecovered(std::filesystem::path OriginalPath)
{
	this->AssertOwnerThread();
	this->Path = std::move(OriginalPath);
	const uint64 CurrentRevision = this->GetRevision();
	this->SavedRevision = CurrentRevision == 1U ? 0U : CurrentRevision - 1U;
}

void SceneDocument::MarkModified() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	const uint64 SceneRevision = this->Scene->GetMutationRevision();
	const uint64 SceneDelta = RevisionDistance(this->RevisionSceneBaseline, SceneRevision);
	this->Revision = AdvanceRevision(this->Revision, SceneDelta == 0 ? 1U : SceneDelta);
	this->RevisionSceneBaseline = SceneRevision;
}

world::Scene &SceneDocument::GetScene() noexcept
{
	return *this->Scene;
}

const world::Scene &SceneDocument::GetScene() const noexcept
{
	return *this->Scene;
}

SelectionSet &SceneDocument::GetSelection() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	return this->Selection;
}

const SelectionSet &SceneDocument::GetSelection() const noexcept
{
	return this->Selection;
}

commands::CommandHistory &SceneDocument::GetHistory() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	return this->History;
}

const util::UUID &SceneDocument::GetID() const noexcept
{
	return this->ID;
}

const string &SceneDocument::GetName() const noexcept
{
	return this->Name;
}

void SceneDocument::SetName(string Name)
{
	this->AssertOwnerThread();
	if (Name.empty())
		throw std::invalid_argument("Scene document name cannot be empty");
	this->Name = std::move(Name);
	this->MarkModified();
}

const std::filesystem::path &SceneDocument::GetPath() const noexcept
{
	return this->Path;
}

uint64 SceneDocument::GetRevision() const noexcept
{
	const uint64 SceneRevision = this->Scene->GetMutationRevision();
	return AdvanceRevision(this->Revision, RevisionDistance(this->RevisionSceneBaseline, SceneRevision));
}

bool SceneDocument::IsDirty() const noexcept
{
	return this->GetRevision() != this->SavedRevision;
}

const string &SceneDocument::GetPreservedSerializationData() const noexcept
{
	return this->PreservedSerializationData;
}

void SceneDocument::SetPreservedSerializationData(string Data)
{
	this->AssertOwnerThread();
	this->PreservedSerializationData = std::move(Data);
}

void SceneDocument::AssertOwnerThread() const
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw std::logic_error("SceneDocument mutation must run on its owner thread");
}
} // namespace editor::document
