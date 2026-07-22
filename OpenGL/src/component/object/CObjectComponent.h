#pragma once

#include "src/component/Components.h"
#include "src/scene/SceneHandles.h"
#include "src/types.h"

namespace world
{
class Scene;
}

namespace components
{
class CObjectComponent
{
	friend class world::Scene;

  private:
	world::ObjectHandle Owner;
	uint32 StorageSlot = world::InvalidSceneSlot;
	uint32 StorageGeneration = 0;
	bool Enabled = true;

  protected:
	explicit CObjectComponent(world::ObjectHandle Owner) noexcept : Owner(Owner)
	{
	}
	CObjectComponent(CObjectComponent &&) noexcept = default;
	CObjectComponent &operator=(CObjectComponent &&) noexcept = default;

  public:
	CObjectComponent() = delete;
	CObjectComponent(const CObjectComponent &) = delete;
	CObjectComponent &operator=(const CObjectComponent &) = delete;
	virtual ~CObjectComponent() noexcept = default;

	[[nodiscard]] bool HasOwner() const noexcept
	{
		return this->Owner.IsValid();
	}
	[[nodiscard]] world::ObjectHandle GetOwner() const noexcept
	{
		return this->Owner;
	}
	[[nodiscard]] bool IsEnabled() const noexcept
	{
		return this->Enabled;
	}
	void SetEnabled(bool Value) noexcept
	{
		this->Enabled = Value;
	}

	[[nodiscard]] virtual uint32 GetTypeID() const noexcept = 0;
	[[nodiscard]] virtual string_view GetComponentName() const noexcept = 0;

	virtual void OnAttachment()
	{
	}
	virtual void OnDetachment()
	{
	}
};
} // namespace components
