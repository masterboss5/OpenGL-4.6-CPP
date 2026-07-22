#pragma once

#include "src/component/ComponentValidation.h"
#include "src/concepts.h"
#include "src/scene/Object.h"
#include "src/scene/SceneException.h"
#include "src/scene/detail/DenseGenerationalPool.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace world
{
struct SceneCapacitySpecification final
{
	uint32 Objects = 65'536;
	uint32 ComponentsPerType = 65'536;
};

namespace detail
{
template <typename List> class ComponentPoolSet;

template <IsCObjectComponent... ComponentTypes> class ComponentPoolSet<TypeList<ComponentTypes...>> final
{
  public:
	explicit ComponentPoolSet(uint32 Capacity) : Pools(std::make_unique<DenseGenerationalPool<ComponentTypes>>(Capacity)...)
	{
	}

	template <IsCObjectComponent T> [[nodiscard]] DenseGenerationalPool<T> &Get() noexcept
	{
		return *std::get<std::unique_ptr<DenseGenerationalPool<T>>>(this->Pools);
	}

	template <IsCObjectComponent T> [[nodiscard]] const DenseGenerationalPool<T> &Get() const noexcept
	{
		return *std::get<std::unique_ptr<DenseGenerationalPool<T>>>(this->Pools);
	}

  private:
	std::tuple<std::unique_ptr<DenseGenerationalPool<ComponentTypes>>...> Pools;
};
} // namespace detail

class Scene final
{
  public:
	class ReadAccess;
	class WriteAccess;

	explicit Scene(SceneCapacitySpecification Capacity = {});
	~Scene() noexcept;
	Scene(const Scene &) = delete;
	Scene &operator=(const Scene &) = delete;
	Scene(Scene &&) = delete;
	Scene &operator=(Scene &&) = delete;

	[[nodiscard]] ObjectHandle CreateObject();
	void DestroyObject(ObjectHandle Object);
	[[nodiscard]] bool Contains(ObjectHandle Object) const;
	[[nodiscard]] ReadAccess Read() const;
	[[nodiscard]] WriteAccess Write();

	template <IsCObjectComponent T, typename... ArgumentTypes>
	[[nodiscard]] ComponentHandle<T> AddComponent(ObjectHandle ObjectHandle, ArgumentTypes &&...Arguments)
	{
		static_assert(TypeListContains<T, components::ComponentTypeList>::Value, "Component must be registered");
		std::unique_lock Lock(this->StructureMutex);
		Object &Object = this->ResolveObjectUnlocked(ObjectHandle);
		if (Object.Components[T::TypeID] != nullptr)
		{
			throw ComponentAlreadyAttachedException(ObjectHandle, T::ComponentName);
		}
		this->ValidateDependencies<T>(Object, components::GetDependencies<T>{});

		auto &Pool = this->ComponentPools.template Get<T>();
		const detail::DensePoolHandle StorageHandle = Pool.Emplace(ObjectHandle, std::forward<ArgumentTypes>(Arguments)...);
		T *Component = Pool.TryResolve(StorageHandle);
		Component->StorageSlot = StorageHandle.Slot;
		Component->StorageGeneration = StorageHandle.Generation;
		Object.Components[T::TypeID] = Component;
		++Object.ComponentsAttached;

		try
		{
			Component->OnAttachment();
		}
		catch (...)
		{
			Object.Components[T::TypeID] = nullptr;
			--Object.ComponentsAttached;
			const auto Relocation = Pool.Erase(StorageHandle);
			this->RepairRelocated<T>(Relocation);
			throw;
		}

		return {.Scene = this->ID, .Slot = StorageHandle.Slot, .Generation = StorageHandle.Generation};
	}

	template <IsCObjectComponent T> void RemoveComponent(ObjectHandle ObjectHandle)
	{
		static_assert(TypeListContains<T, components::ComponentTypeList>::Value, "Component must be registered");
		std::unique_lock Lock(this->StructureMutex);
		Object &Object = this->ResolveObjectUnlocked(ObjectHandle);
		if (Object.Components[T::TypeID] == nullptr)
		{
			throw InvalidComponentHandleException(InvalidSceneSlot, 0, T::ComponentName);
		}
		this->ValidateNoDependents<T>(Object, components::ComponentTypeList{});
		this->DestroyComponentUnchecked<T>(Object);
	}

	template <IsCObjectComponent T> [[nodiscard]] ComponentHandle<T> GetComponent(ObjectHandle ObjectHandle) const
	{
		std::shared_lock Lock(this->StructureMutex);
		const Object &Object = this->ResolveObjectUnlocked(ObjectHandle);
		const auto *Base = Object.Components[T::TypeID];
		if (Base == nullptr)
			return {};
		return {.Scene = this->ID, .Slot = Base->StorageSlot, .Generation = Base->StorageGeneration};
	}

	[[nodiscard]] SceneID GetID() const noexcept
	{
		return this->ID;
	}
	[[nodiscard]] uint32 GetObjectCount() const
	{
		std::shared_lock Lock(this->StructureMutex);
		return this->Objects.Size();
	}

  private:
	SceneID ID = 0;
	detail::DenseGenerationalPool<Object> Objects;
	detail::ComponentPoolSet<components::ComponentTypeList> ComponentPools;
	mutable std::shared_mutex StructureMutex;

	[[nodiscard]] Object &ResolveObjectUnlocked(ObjectHandle Object);
	[[nodiscard]] const Object &ResolveObjectUnlocked(ObjectHandle Object) const;

	template <IsCObjectComponent T> [[nodiscard]] T &ResolveComponentUnlocked(ComponentHandle<T> Handle)
	{
		if (Handle.Scene != this->ID)
			throw InvalidComponentHandleException(Handle.Slot, Handle.Generation, T::ComponentName);
		T *Component = this->ComponentPools.template Get<T>().TryResolve({Handle.Slot, Handle.Generation});
		if (Component == nullptr)
			throw InvalidComponentHandleException(Handle.Slot, Handle.Generation, T::ComponentName);
		return *Component;
	}

	template <IsCObjectComponent T> [[nodiscard]] const T &ResolveComponentUnlocked(ComponentHandle<T> Handle) const
	{
		return const_cast<Scene *>(this)->ResolveComponentUnlocked(Handle);
	}

	template <IsCObjectComponent T, IsCObjectComponent... Dependencies>
	void ValidateDependencies(const Object &Object, TypeList<Dependencies...>) const
	{
		(this->template ValidateDependency<T, Dependencies>(Object), ...);
	}

	template <IsCObjectComponent T, IsCObjectComponent Dependency> void ValidateDependency(const Object &Object) const
	{
		if (Object.Components[Dependency::TypeID] == nullptr)
		{
			throw MissingComponentDependencyException(Object.Self, T::ComponentName, Dependency::ComponentName);
		}
	}

	template <IsCObjectComponent Removed, IsCObjectComponent... Candidates>
	void ValidateNoDependents(const Object &Object, TypeList<Candidates...>) const
	{
		(this->template ValidateDependent<Removed, Candidates>(Object), ...);
	}

	template <IsCObjectComponent Removed, IsCObjectComponent Candidate> void ValidateDependent(const Object &Object) const
	{
		if constexpr (TypeListContains<Removed, components::GetDependencies<Candidate>>::Value)
		{
			if (Object.Components[Candidate::TypeID] != nullptr)
			{
				throw ComponentStillRequiredException(Object.Self, Removed::ComponentName, Candidate::ComponentName);
			}
		}
	}

	template <IsCObjectComponent T> void DestroyComponentUnchecked(Object &Object)
	{
		T *Component = static_cast<T *>(Object.Components[T::TypeID]);
		if (Component == nullptr)
			return;
		Component->OnDetachment();
		const detail::DensePoolHandle StorageHandle{Component->StorageSlot, Component->StorageGeneration};
		Object.Components[T::TypeID] = nullptr;
		--Object.ComponentsAttached;
		const auto Relocation = this->ComponentPools.template Get<T>().Erase(StorageHandle);
		this->RepairRelocated<T>(Relocation);
	}

	template <IsCObjectComponent T> void RepairRelocated(const typename detail::DenseGenerationalPool<T>::Relocation &Relocation)
	{
		if (Relocation.NewAddress == nullptr)
			return;
		Object &Owner = this->ResolveObjectUnlocked(Relocation.NewAddress->Owner);
		Owner.Components[T::TypeID] = Relocation.NewAddress;
	}

	void DestroyAllComponents(Object &, TypeList<>)
	{
	}

	template <IsCObjectComponent First, IsCObjectComponent... Rest> void DestroyAllComponents(Object &Object, TypeList<First, Rest...>)
	{
		this->DestroyAllComponents(Object, TypeList<Rest...>{});
		this->DestroyComponentUnchecked<First>(Object);
	}
};

class Scene::ReadAccess final
{
  public:
	ReadAccess(const ReadAccess &) = delete;
	ReadAccess &operator=(const ReadAccess &) = delete;
	ReadAccess(ReadAccess &&) = delete;
	ReadAccess &operator=(ReadAccess &&) = delete;

	[[nodiscard]] const Object &Resolve(ObjectHandle Object) const
	{
		return this->Owner->ResolveObjectUnlocked(Object);
	}

	template <IsCObjectComponent T> [[nodiscard]] const T &Resolve(ComponentHandle<T> Handle) const
	{
		return this->Owner->ResolveComponentUnlocked(Handle);
	}

	template <IsCObjectComponent T> [[nodiscard]] ComponentHandle<T> GetComponent(ObjectHandle ObjectHandle) const
	{
		const Object &Object = this->Owner->ResolveObjectUnlocked(ObjectHandle);
		const auto *Base = Object.Components[T::TypeID];
		if (Base == nullptr)
			return {};
		return {.Scene = this->Owner->ID, .Slot = Base->StorageSlot, .Generation = Base->StorageGeneration};
	}

	template <IsCObjectComponent T> [[nodiscard]] std::span<const T> Components() const noexcept
	{
		return this->Owner->ComponentPools.template Get<T>().Span();
	}

	[[nodiscard]] uint32 GetObjectCount() const noexcept
	{
		return this->Owner->Objects.Size();
	}

  private:
	friend class Scene;
	explicit ReadAccess(const Scene &Owner) : Owner(&Owner), Lock(Owner.StructureMutex)
	{
	}

	const Scene *Owner = nullptr;
	std::shared_lock<std::shared_mutex> Lock;
};

class Scene::WriteAccess final
{
  public:
	WriteAccess(const WriteAccess &) = delete;
	WriteAccess &operator=(const WriteAccess &) = delete;
	WriteAccess(WriteAccess &&) = delete;
	WriteAccess &operator=(WriteAccess &&) = delete;

	[[nodiscard]] Object &Resolve(ObjectHandle Object) const
	{
		return this->Owner->ResolveObjectUnlocked(Object);
	}

	template <IsCObjectComponent T> [[nodiscard]] T &Resolve(ComponentHandle<T> Handle) const
	{
		return this->Owner->ResolveComponentUnlocked(Handle);
	}

	template <IsCObjectComponent T> [[nodiscard]] ComponentHandle<T> GetComponent(ObjectHandle ObjectHandle) const
	{
		Object &Object = this->Owner->ResolveObjectUnlocked(ObjectHandle);
		auto *Base = Object.Components[T::TypeID];
		if (Base == nullptr)
			return {};
		return {.Scene = this->Owner->ID, .Slot = Base->StorageSlot, .Generation = Base->StorageGeneration};
	}

	template <IsCObjectComponent T> [[nodiscard]] std::span<T> Components() const noexcept
	{
		return this->Owner->ComponentPools.template Get<T>().Span();
	}

	[[nodiscard]] uint32 GetObjectCount() const noexcept
	{
		return this->Owner->Objects.Size();
	}

  private:
	friend class Scene;
	explicit WriteAccess(Scene &Owner) : Owner(&Owner), Lock(Owner.StructureMutex)
	{
	}

	Scene *Owner = nullptr;
	std::unique_lock<std::shared_mutex> Lock;
};
} // namespace world
