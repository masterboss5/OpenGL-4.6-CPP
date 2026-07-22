#pragma once

#include "src/component/object/CObjectComponent.h"
#include "src/resource/asset/MaterialAsset.h"
#include "src/resource/asset/ModelAsset.h"

#include <span>
#include <vector>

namespace components
{
enum class MeshVisibilityFlags : uint32
{
	None = 0,
	Visible = 1U << 0U,
	CastsShadows = 1U << 1U,
	ReceivesShadows = 1U << 2U,
	VisibleInReflections = 1U << 3U
};

[[nodiscard]] constexpr MeshVisibilityFlags operator|(MeshVisibilityFlags Left, MeshVisibilityFlags Right) noexcept
{
	return static_cast<MeshVisibilityFlags>(static_cast<uint32>(Left) | static_cast<uint32>(Right));
}

enum class MeshLODSelectionMode : uint8
{
	Automatic,
	Biased,
	Forced
};

struct MeshLODPolicy final
{
	MeshLODSelectionMode Mode = MeshLODSelectionMode::Automatic;
	int32 Bias = 0;
	uint32 ForcedLOD = 0;
};

struct MeshMaterialOverride final
{
	resource::ModelMeshInstanceID MeshInstance = 0;
	resource::MaterialSlotID MaterialSlot = 0;
	resource::AssetHandle<resource::MaterialInterfaceAsset> Material;
};

class CObjectMeshComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<CObjectTransformComponent>;
	CCOMPONENT_BODY(CObjectMeshComponent)

	explicit CObjectMeshComponent(world::ObjectHandle Owner, resource::AssetHandle<resource::ModelAsset> Model);

	[[nodiscard]] const resource::AssetHandle<resource::ModelAsset> &GetModel() const noexcept
	{
		return this->Model;
	}
	void SetModel(resource::AssetHandle<resource::ModelAsset> Model);
	[[nodiscard]] std::span<const MeshMaterialOverride> GetMaterialOverrides() const noexcept
	{
		return this->MaterialOverrides;
	}
	void SetMaterialOverride(resource::ModelMeshInstanceID MeshInstance, resource::MaterialSlotID MaterialSlot,
							 resource::AssetHandle<resource::MaterialInterfaceAsset> Material);
	void ClearMaterialOverride(resource::ModelMeshInstanceID MeshInstance, resource::MaterialSlotID MaterialSlot) noexcept;
	void ClearMaterialOverrides() noexcept
	{
		this->MaterialOverrides.clear();
	}

	[[nodiscard]] MeshVisibilityFlags GetVisibility() const noexcept
	{
		return this->Visibility;
	}
	void SetVisibility(MeshVisibilityFlags Value) noexcept
	{
		this->Visibility = Value;
	}
	[[nodiscard]] const MeshLODPolicy &GetLODPolicy() const noexcept
	{
		return this->LODPolicy;
	}
	void SetLODPolicy(MeshLODPolicy Value);
	[[nodiscard]] uint32 GetRenderLayerMask() const noexcept
	{
		return this->RenderLayerMask;
	}
	void SetRenderLayerMask(uint32 Value) noexcept
	{
		this->RenderLayerMask = Value;
	}

	void OnAttachment() override;

  private:
	resource::AssetHandle<resource::ModelAsset> Model;
	std::vector<MeshMaterialOverride> MaterialOverrides;
	MeshVisibilityFlags Visibility = MeshVisibilityFlags::Visible | MeshVisibilityFlags::CastsShadows |
									 MeshVisibilityFlags::ReceivesShadows | MeshVisibilityFlags::VisibleInReflections;
	MeshLODPolicy LODPolicy;
	uint32 RenderLayerMask = ~uint32{0};

	void ValidateOverride(resource::ModelMeshInstanceID MeshInstance, resource::MaterialSlotID MaterialSlot,
						  const resource::AssetHandle<resource::MaterialInterfaceAsset> &Material) const;
};
} // namespace components
