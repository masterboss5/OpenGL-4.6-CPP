#include "MeshMaterialOverrideCommand.h"

#include "Source/component/object/CObjectMeshComponent.h"

#include <algorithm>
#include <stdexcept>

namespace editor::commands
{
namespace
{
[[nodiscard]] std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>> FindOverride(
	const components::CObjectMeshComponent &Component, const resource::ModelMeshInstanceID MeshInstance,
	const resource::MaterialSlotID MaterialSlot)
{
	const auto Found = std::ranges::find_if(Component.GetMaterialOverrides(), [&](const components::MeshMaterialOverride &Override)
											{ return Override.MeshInstance == MeshInstance && Override.MaterialSlot == MaterialSlot; });
	return Found == Component.GetMaterialOverrides().end()
			   ? std::nullopt
			   : std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>>(Found->Material);
}

void SetOverride(components::CObjectMeshComponent &Component, const resource::ModelMeshInstanceID MeshInstance,
				 const resource::MaterialSlotID MaterialSlot, const resource::AssetHandle<resource::MaterialInterfaceAsset> &Material)
{
	if (Material)
		Component.SetMaterialOverride(MeshInstance, MaterialSlot, Material);
	else
		Component.ClearMaterialOverride(MeshInstance, MaterialSlot);
}
} // namespace

MeshMaterialOverrideCommand::MeshMaterialOverrideCommand(world::Scene &Scene, const std::span<const world::ObjectHandle> Objects,
														 const resource::ModelMeshInstanceID MeshInstance,
														 const resource::MaterialSlotID MaterialSlot,
														 resource::AssetHandle<resource::MaterialInterfaceAsset> Material)
	: Scene(&Scene), MeshInstance(MeshInstance), MaterialSlot(MaterialSlot), After(std::move(Material))
{
	if (Objects.empty())
		throw std::invalid_argument("A mesh material override requires at least one object");
	if (MeshInstance == 0 || MaterialSlot == 0)
		throw std::invalid_argument("A mesh material override requires valid mesh-instance and material-slot identities");
	if (this->After)
		(void)this->After.Pin();

	this->Targets.reserve(Objects.size());
	for (const world::ObjectHandle Object : Objects)
	{
		if (!Object.IsValid() || !Scene.Contains(Object))
			throw std::invalid_argument("A mesh material override requires live object handles");
		if (std::ranges::find(this->Targets, Object, &TargetState::Object) != this->Targets.end())
			continue;
		const auto Handle = Scene.GetComponent<components::CObjectMeshComponent>(Object);
		if (!Handle.IsValid())
			throw std::invalid_argument("A mesh material override target requires a mesh component");
		auto Access = Scene.Read();
		this->Targets.push_back({.Object = Object, .Before = FindOverride(Access.Resolve(Handle), MeshInstance, MaterialSlot)});
	}
}

string_view MeshMaterialOverrideCommand::GetName() const noexcept
{
	return "Set Mesh Material";
}

void MeshMaterialOverrideCommand::Execute()
{
	this->ApplyUniform(this->After);
}

void MeshMaterialOverrideCommand::Undo()
{
	this->RestoreBefore();
}

bool MeshMaterialOverrideCommand::TryMerge(const EditorCommand &Other)
{
	const auto *Typed = dynamic_cast<const MeshMaterialOverrideCommand *>(&Other);
	if (Typed == nullptr || Typed->Scene != this->Scene || Typed->MeshInstance != this->MeshInstance ||
		Typed->MaterialSlot != this->MaterialSlot || Typed->Targets.size() != this->Targets.size())
		return false;
	for (usize Index = 0; Index < this->Targets.size(); ++Index)
	{
		if (this->Targets[Index].Object != Typed->Targets[Index].Object)
			return false;
	}
	this->After = Typed->After;
	return true;
}

void MeshMaterialOverrideCommand::ApplyUniform(const resource::AssetHandle<resource::MaterialInterfaceAsset> &Material)
{
	for (const TargetState &Target : this->Targets)
	{
		if (!this->Scene->Contains(Target.Object))
			throw std::out_of_range("Mesh material override target no longer exists");
	}
	auto Access = this->Scene->Write();
	std::vector<std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>>> Rollback;
	Rollback.reserve(this->Targets.size());
	for (const TargetState &Target : this->Targets)
	{
		const auto Handle = Access.GetComponent<components::CObjectMeshComponent>(Target.Object);
		Rollback.push_back(FindOverride(Access.Resolve(Handle), this->MeshInstance, this->MaterialSlot));
	}
	usize Applied = 0;
	try
	{
		for (; Applied < this->Targets.size(); ++Applied)
		{
			auto &Component = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(this->Targets[Applied].Object));
			SetOverride(Component, this->MeshInstance, this->MaterialSlot, Material);
		}
	}
	catch (...)
	{
		while (Applied != 0)
		{
			--Applied;
			auto &Component = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(this->Targets[Applied].Object));
			SetOverride(Component, this->MeshInstance, this->MaterialSlot, Rollback[Applied].value_or(nullptr));
		}
		throw;
	}
}

void MeshMaterialOverrideCommand::RestoreBefore()
{
	for (const TargetState &Target : this->Targets)
	{
		if (!this->Scene->Contains(Target.Object))
			throw std::out_of_range("Mesh material override target no longer exists");
	}
	auto Access = this->Scene->Write();
	for (const TargetState &Target : this->Targets)
	{
		auto &Component = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Target.Object));
		SetOverride(Component, this->MeshInstance, this->MaterialSlot, Target.Before.value_or(nullptr));
	}
}
} // namespace editor::commands
