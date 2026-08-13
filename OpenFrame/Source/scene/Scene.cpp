#include "Scene.h"

#include "SceneTransformSnapshot.h"

#include <atomic>

namespace
{
std::atomic<uint64> NextSceneID{1};
}

namespace world
{
Scene::Scene(SceneCapacitySpecification Capacity)
	: ID(NextSceneID.fetch_add(1, std::memory_order_relaxed)), Objects(Capacity.Objects), ComponentPools(Capacity.ComponentsPerType)
{
	if (this->ID == 0)
	{
		throw SceneCapacityException("Scene identity space has been exhausted");
	}
}

Scene::~Scene() noexcept
{
	try
	{
		std::unique_lock Lock(this->StructureMutex);
		while (this->Objects.Size() != 0)
		{
			const detail::DensePoolHandle StorageHandle = this->Objects.HandleAtDense(this->Objects.Size() - 1);
			Object *Object = this->Objects.TryResolve(StorageHandle);
			this->DestroyAllComponents(*Object, components::ComponentTypeList{});
			(void)this->Objects.Erase(StorageHandle);
		}
	}
	catch (...)
	{
		std::terminate();
	}
}

ObjectHandle Scene::CreateObject()
{
	std::unique_lock Lock(this->StructureMutex);
	const detail::DensePoolHandle StorageHandle = this->Objects.Emplace(ObjectHandle{});
	Object *Object = this->Objects.TryResolve(StorageHandle);
	Object->Self = {.Scene = this->ID, .Slot = StorageHandle.Slot, .Generation = StorageHandle.Generation};
	this->MarkMutatedUnlocked();
	return Object->Self;
}

void Scene::DestroyObject(ObjectHandle ObjectHandle)
{
	std::unique_lock Lock(this->StructureMutex);
	Object &Object = this->ResolveObjectUnlocked(ObjectHandle);
	for (components::CObjectHierarchyComponent &Hierarchy : this->ComponentPools.Get<components::CObjectHierarchyComponent>().Span())
	{
		if (Hierarchy.Parent == ObjectHandle)
			Hierarchy.Parent = {};
	}
	this->DestroyAllComponents(Object, components::ComponentTypeList{});
	(void)this->Objects.Erase({ObjectHandle.Slot, ObjectHandle.Generation});
	this->MarkMutatedUnlocked();
}

void Scene::SetParent(const ObjectHandle Child, const ObjectHandle Parent, const uint32 SiblingOrder)
{
	std::unique_lock Lock(this->StructureMutex);
	Object &ChildObject = this->ResolveObjectUnlocked(Child);
	auto *Hierarchy =
		static_cast<components::CObjectHierarchyComponent *>(ChildObject.Components[components::CObjectHierarchyComponent::TypeID]);
	if (Hierarchy == nullptr)
		throw InvalidComponentHandleException(InvalidSceneSlot, 0, components::CObjectHierarchyComponent::ComponentName);
	if (Parent == Child)
		throw SceneException("An object cannot be parented to itself");

	if (Parent.IsValid())
	{
		const Object &ParentObject = this->ResolveObjectUnlocked(Parent);
		if (ParentObject.Components[components::CObjectHierarchyComponent::TypeID] == nullptr)
			throw MissingComponentDependencyException(Parent, components::CObjectHierarchyComponent::ComponentName,
													  components::CObjectHierarchyComponent::ComponentName);

		ObjectHandle Ancestor = Parent;
		while (Ancestor.IsValid())
		{
			if (Ancestor == Child)
				throw SceneException("Object hierarchy cannot contain a cycle");
			const Object &AncestorObject = this->ResolveObjectUnlocked(Ancestor);
			const auto *AncestorHierarchy = static_cast<const components::CObjectHierarchyComponent *>(
				AncestorObject.Components[components::CObjectHierarchyComponent::TypeID]);
			Ancestor = AncestorHierarchy == nullptr ? ObjectHandle{} : AncestorHierarchy->Parent;
		}
	}

	Hierarchy->Parent = Parent;
	Hierarchy->SiblingOrder = SiblingOrder;
	this->MarkMutatedUnlocked();
}

bool Scene::Contains(ObjectHandle Object) const
{
	if (Object.Scene != this->ID)
		return false;
	std::shared_lock Lock(this->StructureMutex);
	return this->Objects.Contains({Object.Slot, Object.Generation});
}

std::vector<ObjectHandle> Scene::GetObjects() const
{
	return this->Read().Objects();
}

ObjectHandle Scene::FindObject(const util::UUID &PersistentID) const
{
	std::shared_lock Lock(this->StructureMutex);
	for (const components::CObjectIdentityComponent &Identity : this->ComponentPools.Get<components::CObjectIdentityComponent>().Span())
	{
		if (Identity.GetPersistentID() == PersistentID)
			return Identity.GetOwner();
	}
	return {};
}

Scene::ReadAccess Scene::Read() const
{
	return ReadAccess(*this);
}

glm::mat4 Scene::ReadAccess::GetWorldTransform(const ObjectHandle Object) const
{
	return SceneTransformSnapshot::Build(*this).GetMatrix(Object);
}

Scene::WriteAccess Scene::Write()
{
	return WriteAccess(*this, true);
}

Scene::WriteAccess Scene::WriteTransient()
{
	return WriteAccess(*this, false);
}

Object &Scene::ResolveObjectUnlocked(ObjectHandle Object)
{
	if (Object.Scene != this->ID)
	{
		throw InvalidObjectHandleException(Object);
	}
	world::Object *Resolved = this->Objects.TryResolve({Object.Slot, Object.Generation});
	if (Resolved == nullptr)
	{
		throw InvalidObjectHandleException(Object);
	}
	return *Resolved;
}

const Object &Scene::ResolveObjectUnlocked(ObjectHandle Object) const
{
	return const_cast<Scene *>(this)->ResolveObjectUnlocked(Object);
}
} // namespace world

namespace world
{
components::CObjectComponent &Scene::ResolveComponentUnlocked(const ObjectHandle ObjectHandle, const uint32 ComponentType)
{
	if (ComponentType >= components::CObjectComponents)
		throw std::out_of_range("Component type identity exceeds the registered component range");
	Object &ResolvedObject = this->ResolveObjectUnlocked(ObjectHandle);
	components::CObjectComponent *Component = ResolvedObject.Components[ComponentType];
	if (Component == nullptr)
		throw std::out_of_range("Object does not contain the requested component type");
	return *Component;
}

const components::CObjectComponent &Scene::ResolveComponentUnlocked(const ObjectHandle ObjectHandle, const uint32 ComponentType) const
{
	return const_cast<Scene *>(this)->ResolveComponentUnlocked(ObjectHandle, ComponentType);
}
} // namespace world
