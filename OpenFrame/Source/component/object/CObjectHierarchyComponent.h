#pragma once

#include "Source/component/object/CObjectComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/types.h"

namespace components
{
class ENGINE_API CObjectHierarchyComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<CObjectIdentityComponent, CObjectTransformComponent>;

	explicit CObjectHierarchyComponent(world::ObjectHandle Owner) noexcept;
	CCOMPONENT_BODY(CObjectHierarchyComponent)

	[[nodiscard]] world::ObjectHandle GetParent() const noexcept;
	[[nodiscard]] uint32 GetSiblingOrder() const noexcept;

  private:
	friend class world::Scene;

	world::ObjectHandle Parent;
	uint32 SiblingOrder = 0;
};
} // namespace components
