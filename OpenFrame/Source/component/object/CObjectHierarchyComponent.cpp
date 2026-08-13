#include "CObjectHierarchyComponent.h"

namespace components
{
CObjectHierarchyComponent::CObjectHierarchyComponent(const world::ObjectHandle Owner) noexcept : CObjectComponent(Owner)
{
}

world::ObjectHandle CObjectHierarchyComponent::GetParent() const noexcept
{
	return this->Parent;
}

uint32 CObjectHierarchyComponent::GetSiblingOrder() const noexcept
{
	return this->SiblingOrder;
}
} // namespace components
