#include "CreatePrimitiveCommand.h"

#include "src/component/object/CObjectMeshComponent.h"
#include "src/editor/document/SceneDocument.h"

#include <stdexcept>

namespace editor::commands
{
CreatePrimitiveCommand::CreatePrimitiveCommand(document::SceneDocument &Document, const asset::PrimitiveShape Shape,
											   resource::AssetHandle<resource::ModelAsset> Model, const util::UUID Parent)
	: Document(&Document), Shape(Shape), Model(std::move(Model)), Parent(Parent), PreviousSelection(Document.GetSelection().GetOrdered())
{
	if (static_cast<usize>(this->Shape) >= static_cast<usize>(asset::PrimitiveShape::Count))
		throw std::invalid_argument("CreatePrimitiveCommand requires a concrete primitive shape");
	if (!this->Model)
		throw std::invalid_argument("CreatePrimitiveCommand requires a valid model asset");
	if (this->Model.GetID() != asset::PrimitiveMeshFactory::GetModelID(this->Shape))
		throw std::invalid_argument("CreatePrimitiveCommand model does not match the requested primitive shape");
	if (this->Parent.IsValid() && !Document.GetScene().FindObject(this->Parent).IsValid())
		throw std::out_of_range("CreatePrimitiveCommand parent does not exist");
}

string_view CreatePrimitiveCommand::GetName() const noexcept
{
	return "Create Primitive";
}

void CreatePrimitiveCommand::Execute()
{
	if (this->Present)
		throw std::logic_error("CreatePrimitiveCommand cannot create an object that is already present");

	const world::ObjectHandle Parent = this->Parent.IsValid() ? this->Document->GetScene().FindObject(this->Parent) : world::ObjectHandle{};
	if (this->Parent.IsValid() && !Parent.IsValid())
		throw std::out_of_range("CreatePrimitiveCommand parent is no longer present");
	const world::ObjectHandle Object =
		this->Document->CreateObject(string(asset::PrimitiveMeshFactory::GetName(this->Shape)), Parent, this->PersistentID);
	try
	{
		(void)this->Document->GetScene().AddComponent<components::CObjectMeshComponent>(Object, this->Model);
		this->Present = true;
	}
	catch (...)
	{
		this->Document->DestroyObject(this->PersistentID);
		throw;
	}
}

void CreatePrimitiveCommand::Undo()
{
	if (!this->Present)
		throw std::logic_error("CreatePrimitiveCommand cannot remove an object that is not present");
	this->Document->DestroyObject(this->PersistentID);
	this->Document->GetSelection().Clear();
	for (const util::UUID &ID : this->PreviousSelection)
	{
		if (this->Document->GetScene().FindObject(ID).IsValid())
			this->Document->GetSelection().Add(ID);
	}
	this->Present = false;
}

const util::UUID &CreatePrimitiveCommand::GetPersistentID() const noexcept
{
	return this->PersistentID;
}
} // namespace editor::commands
