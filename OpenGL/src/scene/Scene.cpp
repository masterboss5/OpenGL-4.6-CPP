#include "Scene.h"

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
	return Object->Self;
}

void Scene::DestroyObject(ObjectHandle ObjectHandle)
{
	std::unique_lock Lock(this->StructureMutex);
	Object &Object = this->ResolveObjectUnlocked(ObjectHandle);
	this->DestroyAllComponents(Object, components::ComponentTypeList{});
	(void)this->Objects.Erase({ObjectHandle.Slot, ObjectHandle.Generation});
}

bool Scene::Contains(ObjectHandle Object) const
{
	if (Object.Scene != this->ID)
		return false;
	std::shared_lock Lock(this->StructureMutex);
	return this->Objects.Contains({Object.Slot, Object.Generation});
}

Scene::ReadAccess Scene::Read() const
{
	return ReadAccess(*this);
}

Scene::WriteAccess Scene::Write()
{
	return WriteAccess(*this);
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
