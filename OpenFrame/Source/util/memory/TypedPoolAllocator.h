#pragma once
#include "Source/concepts.h"
#include "Source/types.h"

#include <cassert>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace memory
{
template <PoolAllocatable T> class TypedPoolAllocator final
{
  private:
	std::allocator<T> Allocator;
	T *Storage{nullptr};
	const usize Capacity;
	usize Count{0};
	const std::thread::id OwnerThread{std::this_thread::get_id()};

	void RequireOwnerThread() const
	{
		if (std::this_thread::get_id() != this->OwnerThread)
		{
			throw std::logic_error("TypedPoolAllocator may only be accessed by its owner thread");
		}
	}

  public:
	explicit TypedPoolAllocator(usize Capacity) : Capacity(Capacity)
	{
		if (Capacity == 0)
		{
			throw std::invalid_argument("TypedPoolAllocator capacity must be greater than zero");
		}

		if (Capacity > std::numeric_limits<usize>::max() / sizeof(T))
		{
			throw std::bad_array_new_length{};
		}

		this->Storage = std::allocator_traits<std::allocator<T>>::allocate(this->Allocator, Capacity);
	}

	~TypedPoolAllocator() noexcept
	{
		if (std::this_thread::get_id() != this->OwnerThread)
		{
			std::terminate();
		}
		for (usize Index = this->Count; Index != 0U; --Index)
		{
			std::destroy_at(this->Storage + Index - 1U);
		}
		std::allocator_traits<std::allocator<T>>::deallocate(this->Allocator, this->Storage, this->Capacity);
	}

	TypedPoolAllocator(const TypedPoolAllocator &) = delete;
	TypedPoolAllocator &operator=(const TypedPoolAllocator &) = delete;
	TypedPoolAllocator(TypedPoolAllocator &&) = delete;
	TypedPoolAllocator &operator=(TypedPoolAllocator &&) = delete;

	template <typename... ArgumentTypes> [[nodiscard]] T *Allocate(ArgumentTypes &&...Arguments)
	{
		this->RequireOwnerThread();
		if (this->Count >= this->Capacity)
		{
			throw std::bad_alloc{};
		}

		T *Slot = this->Storage + this->Count;
		std::construct_at(Slot, std::forward<ArgumentTypes>(Arguments)...);
		this->Count++;

		return Slot;
	}

	[[nodiscard]] usize GetCount() const
	{
		this->RequireOwnerThread();
		return this->Count;
	}

	[[nodiscard]] usize GetCapacity() const
	{
		this->RequireOwnerThread();
		return this->Capacity;
	}

	[[nodiscard]] usize GetRemainingCapacity() const
	{
		this->RequireOwnerThread();
		return this->Capacity - this->Count;
	}

	[[nodiscard]] bool IsFull() const
	{
		this->RequireOwnerThread();
		return this->Count >= this->Capacity;
	}

	[[nodiscard]] T &operator[](usize Index)
	{
		this->RequireOwnerThread();
		assert(Index < this->Count);
		return this->Storage[Index];
	}

	[[nodiscard]] T &At(usize Index)
	{
		this->RequireOwnerThread();
		if (Index >= this->Count)
		{
			throw std::out_of_range("TypedPoolAllocator::at index out of range");
		}

		return this->Storage[Index];
	}

	[[nodiscard]] const T &operator[](usize Index) const
	{
		this->RequireOwnerThread();
		assert(Index < this->Count);
		return this->Storage[Index];
	}

	[[nodiscard]] const T &At(usize Index) const
	{
		this->RequireOwnerThread();
		if (Index >= this->Count)
		{
			throw std::out_of_range{"TypedPoolAllocator::at index out of range"};
		}

		return this->Storage[Index];
	}

	[[nodiscard]] bool IsEmpty() const
	{
		this->RequireOwnerThread();
		return this->Count == 0;
	}

	[[nodiscard]] bool Contains(const T *Ptr) const
	{
		this->RequireOwnerThread();
		if (Ptr == nullptr)
		{
			return false;
		}

		const usize Address = reinterpret_cast<usize>(Ptr);
		const usize Begin = reinterpret_cast<usize>(this->Storage);
		const usize LiveEnd = Begin + this->Count * sizeof(T);
		return Address >= Begin && Address < LiveEnd && (Address - Begin) % sizeof(T) == 0U;
	}

	// Reports ownership of raw backing storage, including storage that does not
	// currently contain a live T. Contains() is the live-object-start query.
	[[nodiscard]] bool OwnsStorageAddress(const void *Ptr) const
	{
		this->RequireOwnerThread();
		if (Ptr == nullptr)
		{
			return false;
		}

		const usize Address = reinterpret_cast<usize>(Ptr);
		const usize Begin = reinterpret_cast<usize>(this->Storage);
		const usize StorageEnd = Begin + this->Capacity * sizeof(T);
		return Address >= Begin && Address < StorageEnd;
	}

	void Reset()
	{
		// Reset invalidates every pointer, reference, and span previously returned.
		this->RequireOwnerThread();
		for (usize I = this->Count; I > 0; --I)
		{
			std::destroy_at(this->Storage + I - 1U);
		}

		this->Count = 0;
	}

	[[nodiscard]] std::span<T> Span()
	{
		this->RequireOwnerThread();
		return std::span<T>(this->Storage, this->Count);
	}

	[[nodiscard]] std::span<const T> Span() const
	{
		this->RequireOwnerThread();
		return std::span<const T>(this->Storage, this->Count);
	}
};
} // namespace memory
