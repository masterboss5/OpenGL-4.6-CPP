#pragma once
#include "src/concepts.h"
#include "src/types.h"

#include <cassert>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace memory
{
template <PoolAllocatable T> class TypedPoolAllocator final
{
  private:
	std::byte *Buffer{nullptr};
	const usize Capacity;
	usize Count{0};

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

		this->Buffer = static_cast<std::byte *>(::operator new(Capacity * sizeof(T), std::align_val_t{alignof(T)}));
	}

	~TypedPoolAllocator() noexcept
	{
		this->Reset();

		if (this->Buffer)
		{
			::operator delete(this->Buffer, this->Capacity * sizeof(T), std::align_val_t{alignof(T)});
		}
	}

	TypedPoolAllocator(const TypedPoolAllocator &) = delete;
	TypedPoolAllocator &operator=(const TypedPoolAllocator &) = delete;
	TypedPoolAllocator(TypedPoolAllocator &&) = delete;
	TypedPoolAllocator &operator=(TypedPoolAllocator &&) = delete;

	template <typename... ArgumentTypes> [[nodiscard]] T *Allocate(ArgumentTypes &&...Arguments)
	{
		if (this->Count >= this->Capacity)
		{
			throw std::bad_alloc{};
		}

		T *Slot = reinterpret_cast<T *>(this->Buffer + this->Count * sizeof(T));
		::new (Slot) T(std::forward<ArgumentTypes>(Arguments)...);
		this->Count++;

		return Slot;
	}

	[[nodiscard]] usize GetCount() const noexcept
	{
		return this->Count;
	}

	[[nodiscard]] usize GetCapacity() const noexcept
	{
		return this->Capacity;
	}

	[[nodiscard]] usize GetRemainingCapacity() const noexcept
	{
		return this->Capacity - this->Count;
	}

	[[nodiscard]] bool IsFull() const noexcept
	{
		return this->Count >= this->Capacity;
	}

	[[nodiscard]] T &operator[](usize Index) noexcept
	{
		assert(Index < this->Count);
		return *reinterpret_cast<T *>(this->Buffer + Index * sizeof(T));
	}

	[[nodiscard]] T &At(usize Index)
	{
		if (Index >= this->Count)
		{
			throw std::out_of_range("TypedPoolAllocator::at index out of range");
		}

		return *reinterpret_cast<T *>(this->Buffer + Index * sizeof(T));
	}

	[[nodiscard]] const T &operator[](usize Index) const noexcept
	{
		assert(Index < this->Count);
		return *reinterpret_cast<const T *>(this->Buffer + Index * sizeof(T));
	}

	[[nodiscard]] const T &At(usize Index) const
	{
		if (Index >= this->Count)
		{
			throw std::out_of_range{"TypedPoolAllocator::at index out of range"};
		}

		return *reinterpret_cast<const T *>(this->Buffer + Index * sizeof(T));
	}

	[[nodiscard]] bool IsEmpty() const noexcept
	{
		return this->Count == 0;
	}

	[[nodiscard]] bool Contains(const T *Ptr) const noexcept
	{
		if (Ptr == nullptr)
		{
			return false;
		}

		const T *Start = reinterpret_cast<const T *>(this->Buffer);
		const T *End = Start + this->Count;

		return std::less<const T *>{}(Ptr, End) && !std::less<const T *>{}(Ptr, Start);
	}

	void Reset() noexcept
	{
		for (usize I = this->Count; I > 0; --I)
		{
			T *Ptr = reinterpret_cast<T *>(this->Buffer + (I - 1) * sizeof(T));
			Ptr->~T();
		}

		this->Count = 0;
	}

	[[nodiscard]] std::span<T> Span() noexcept
	{
		return std::span<T>(reinterpret_cast<T *>(this->Buffer), this->Count);
	}

	[[nodiscard]] std::span<const T> Span() const noexcept
	{
		return std::span<const T>(reinterpret_cast<const T *>(this->Buffer), this->Count);
	}
};
} // namespace memory
