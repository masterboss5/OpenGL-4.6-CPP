#include "CObjectMeshComponent.h"

#include <algorithm>
#include <stdexcept>

namespace components
{
CObjectMeshComponent::CObjectMeshComponent(world::ObjectHandle Owner, resource::AssetHandle<resource::ModelAsset> Model)
	: CObjectComponent(Owner), Model(std::move(Model))
{
	if (!this->Model)
		throw std::invalid_argument("Mesh component requires a ModelAsset handle");
}

void CObjectMeshComponent::SetModel(resource::AssetHandle<resource::ModelAsset> Replacement)
{
	if (!Replacement)
		throw std::invalid_argument("Mesh component requires a ModelAsset handle");
	(void)Replacement.Pin();
	this->Model = std::move(Replacement);
	this->MaterialOverrides.clear();
}

void CObjectMeshComponent::SetMaterialOverride(resource::ModelMeshInstanceID MeshInstance, resource::MaterialSlotID MaterialSlot,
											   resource::AssetHandle<resource::MaterialInterfaceAsset> Material)
{
	this->ValidateOverride(MeshInstance, MaterialSlot, Material);
	const auto KeyLess = [](const MeshMaterialOverride &Left, const MeshMaterialOverride &Right)
	{
		return Left.MeshInstance < Right.MeshInstance ||
			   (Left.MeshInstance == Right.MeshInstance && Left.MaterialSlot < Right.MaterialSlot);
	};
	MeshMaterialOverride Replacement{MeshInstance, MaterialSlot, std::move(Material)};
	auto Position = std::lower_bound(this->MaterialOverrides.begin(), this->MaterialOverrides.end(), Replacement, KeyLess);
	if (Position != this->MaterialOverrides.end() && Position->MeshInstance == MeshInstance && Position->MaterialSlot == MaterialSlot)
	{
		*Position = std::move(Replacement);
	}
	else
	{
		this->MaterialOverrides.insert(Position, std::move(Replacement));
	}
}

void CObjectMeshComponent::ClearMaterialOverride(resource::ModelMeshInstanceID MeshInstance, resource::MaterialSlotID MaterialSlot) noexcept
{
	const auto Found = std::find_if(this->MaterialOverrides.begin(), this->MaterialOverrides.end(),
									[MeshInstance, MaterialSlot](const MeshMaterialOverride &Value)
									{ return Value.MeshInstance == MeshInstance && Value.MaterialSlot == MaterialSlot; });
	if (Found != this->MaterialOverrides.end())
		this->MaterialOverrides.erase(Found);
}

void CObjectMeshComponent::SetLODPolicy(MeshLODPolicy Value)
{
	if (Value.Mode == MeshLODSelectionMode::Automatic && Value.Bias != 0)
	{
		throw std::invalid_argument("Automatic mesh LOD policy cannot carry a bias");
	}
	this->LODPolicy = Value;
}

void CObjectMeshComponent::OnAttachment()
{
	(void)this->Model.Pin();
}

void CObjectMeshComponent::ValidateOverride(resource::ModelMeshInstanceID MeshInstanceID, resource::MaterialSlotID MaterialSlot,
											const resource::AssetHandle<resource::MaterialInterfaceAsset> &Material) const
{
	if (!Material)
		throw std::invalid_argument("Mesh material override requires a material handle");
	(void)Material.Pin();
	auto PinnedModel = this->Model.Pin();
	const resource::ModelMeshInstance *Instance = PinnedModel->FindMeshInstance(MeshInstanceID);
	if (Instance == nullptr)
		throw std::invalid_argument("Mesh material override references a missing model mesh instance");
	auto PinnedMesh = Instance->Mesh.Pin();
	if (PinnedMesh->FindMaterialSlot(MaterialSlot) == nullptr)
	{
		throw std::invalid_argument("Mesh material override references a missing mesh material slot");
	}
}
} // namespace components
