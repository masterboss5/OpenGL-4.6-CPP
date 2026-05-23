#pragma once
#include <utility>
#include <cassert>
#include "src/Types.h"
#include <cstddef>
#include <new>
#include <type_traits>
#include <stdexcept>
#include <limits>
#include <span>

namespace memory
{
	template<typename T>
	class TypedPoolAllocator final
	{
		static_assert(std::is_nothrow_destructible_v<T>,
			"TypedPoolAllocator requires T to be nothrow destructible");

		static_assert(sizeof(T) > 0,
			"T must be greater than 0 bytes");

		static_assert(std::is_object_v<T>,
			"T must be an object");

		static_assert(!std::is_array_v<T>,
			"TypedPoolAllocator does not support array element types");

		static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>,
			"TypedPoolAllocator requires non-cv-qualified T");

		static_assert(!std::is_abstract_v<T>,
			"TypedPoolAllocator does not support abstract types");

	private:
		std::byte* buffer {nullptr};
		const std::size_t capacity;
		std::size_t count {0};
	public:
		explicit TypedPoolAllocator(std::size_t capacity)
			: capacity(capacity)
		{
			if (capacity == 0)
			{
				throw std::invalid_argument("TypedPoolAllocator capacity must be greater than zero");
			}

			if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(T))
			{
				throw std::bad_array_new_length{};
			}

			this->buffer = static_cast<std::byte*>(
				::operator new(capacity * sizeof(T), std::align_val_t{alignof(T)})
			);
		}

		~TypedPoolAllocator() noexcept
		{
			this->reset();

			if (this->buffer)
			{
				::operator delete(this->buffer, this->capacity * sizeof(T), std::align_val_t{alignof(T)});
			}
		}

		TypedPoolAllocator(const TypedPoolAllocator&) = delete;
		TypedPoolAllocator& operator=(const TypedPoolAllocator&) = delete;
		TypedPoolAllocator(TypedPoolAllocator&&) = delete;
		TypedPoolAllocator& operator=(TypedPoolAllocator&&) = delete;

		template<typename... Args>
		[[nodiscard]] T* allocate(Args&&... args)
		{
			if (this->count >= this->capacity)
			{
				throw std::bad_alloc {};
			}

			T* slot = reinterpret_cast<T*>(this->buffer + this->count * sizeof(T));
			::new (slot) T(std::forward<Args>(args)...);
			this->count++;

			return slot;
		}

		[[nodiscard]] std::size_t getCount() const noexcept
		{
			return this->count;
		}

		[[nodiscard]] std::size_t getCapacity() const noexcept
		{
			return this->capacity;
		}

		[[nodiscard]] std::size_t getRemainingCapacity() const noexcept
		{
			return this->capacity - this->count;
		}

		[[nodiscard]] bool isFull() const noexcept
		{
			return this->count >= this->capacity;
		}

		[[nodiscard]] T& operator[](std::size_t index) noexcept
		{
			assert(index < this->count);
			return *reinterpret_cast<T*>(this->buffer + index * sizeof(T));
		}

		[[nodiscard]] T& at(std::size_t index)
		{
			if (index >= this->count)
			{
				throw std::out_of_range("TypedPoolAllocator::at index out of range");
			}

			return *reinterpret_cast<T*>(this->buffer + index * sizeof(T));
		}

		[[nodiscard]] const T& operator[](std::size_t index) const noexcept
		{
			assert(index < this->count);
			return *reinterpret_cast<const T*>(this->buffer + index * sizeof(T));
		}

		[[nodiscard]] const T& at(std::size_t index) const
		{
			if (index >= this->count)
			{
				throw std::out_of_range {"TypedPoolAllocator::at index out of range"};
			}

			return *reinterpret_cast<const T*>(this->buffer + index * sizeof(T));
		}

		[[nodiscard]] bool isEmpty() const noexcept
		{
			return this->count == 0;
		}

		[[nodiscard]] bool contains(const T* ptr) const noexcept
		{
			if (ptr == nullptr)
			{
				return false;
			}

			const T* start = reinterpret_cast<const T*>(this->buffer);
			const T* end = start + this->count;

			return std::less<const T*>{}(ptr, end) && !std::less<const T*>{}(ptr, start);
		}

		void reset() noexcept
		{
			for (std::size_t i = this->count; i > 0; --i)
			{
				T* ptr = reinterpret_cast<T*>(this->buffer + (i - 1) * sizeof(T));
				ptr->~T();
			}

			this->count = 0;
		}

		[[nodiscard]] std::span<T> span() noexcept
		{
			return std::span<T>(
				reinterpret_cast<T*>(this->buffer),
				this->count
			);
		}

		[[nodiscard]] std::span<const T> span() const noexcept
		{
			return std::span<const T>(
				reinterpret_cast<const T*>(this->buffer),
				this->count
			);
		}
	};
}
