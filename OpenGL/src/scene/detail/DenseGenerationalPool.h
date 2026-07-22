#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "src/scene/SceneException.h"
#include "src/types.h"

#include <Windows.h>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace world::detail
{
struct DensePoolHandle final
{
	uint32 Slot = InvalidSceneSlot;
	uint32 Generation = 0;
};

template <typename T> class DenseGenerationalPool final
{
	static_assert(std::is_nothrow_destructible_v<T>, "Scene pool elements must be nothrow destructible");
	static_assert(std::is_nothrow_move_constructible_v<T>, "Scene pool elements must be nothrow move constructible");

  public:
	struct Relocation final
	{
		T *OldAddress = nullptr;
		T *NewAddress = nullptr;
		DensePoolHandle Handle;
	};

	explicit DenseGenerationalPool(uint32 Capacity)
		: CapacityValue(Capacity), SlotToDense(Capacity, InvalidSceneSlot), DenseToSlot(Capacity, InvalidSceneSlot),
		  Generations(Capacity, 1)
	{
		if (Capacity == 0)
		{
			throw SceneCapacityException("Scene pool capacity must be greater than zero");
		}
		if (static_cast<uint64>(Capacity) * static_cast<uint64>(sizeof(T)) > static_cast<uint64>(std::numeric_limits<usize>::max()))
		{
			throw SceneCapacityException("Scene pool reservation exceeds addressable memory");
		}

		SYSTEM_INFO SystemInfo{};
		GetSystemInfo(&SystemInfo);
		this->PageSize = static_cast<usize>(SystemInfo.dwPageSize);
		this->ReservedBytes = static_cast<usize>(Capacity) * sizeof(T);
		this->Storage = static_cast<T *>(VirtualAlloc(nullptr, this->ReservedBytes, MEM_RESERVE, PAGE_READWRITE));
		if (this->Storage == nullptr)
		{
			throw SceneCapacityException("Failed to reserve contiguous Scene pool address space");
		}
		this->FreeSlots.reserve(Capacity);
	}

	~DenseGenerationalPool() noexcept
	{
		this->Clear();
		if (this->Storage != nullptr)
		{
			VirtualFree(this->Storage, 0, MEM_RELEASE);
			this->Storage = nullptr;
		}
	}

	DenseGenerationalPool(const DenseGenerationalPool &) = delete;
	DenseGenerationalPool &operator=(const DenseGenerationalPool &) = delete;
	DenseGenerationalPool(DenseGenerationalPool &&) = delete;
	DenseGenerationalPool &operator=(DenseGenerationalPool &&) = delete;

	template <typename... ArgumentTypes> [[nodiscard]] DensePoolHandle Emplace(ArgumentTypes &&...Arguments)
	{
		if (this->DenseCount == this->CapacityValue)
		{
			throw SceneCapacityException("Scene pool has reached its configured capacity");
		}
		this->CommitFor(this->DenseCount + 1);

		const bool Recycled = !this->FreeSlots.empty();
		uint32 Slot = 0;
		if (Recycled)
		{
			Slot = this->FreeSlots.back();
			this->FreeSlots.pop_back();
		}
		else
		{
			Slot = this->NextUnusedSlot++;
		}

		T *Destination = this->Storage + this->DenseCount;
		try
		{
			::new (static_cast<void *>(Destination)) T(std::forward<ArgumentTypes>(Arguments)...);
		}
		catch (...)
		{
			if (Recycled)
				this->FreeSlots.push_back(Slot);
			else
				--this->NextUnusedSlot;
			throw;
		}

		this->SlotToDense[Slot] = this->DenseCount;
		this->DenseToSlot[this->DenseCount] = Slot;
		++this->DenseCount;
		return {.Slot = Slot, .Generation = this->Generations[Slot]};
	}

	[[nodiscard]] Relocation Erase(DensePoolHandle Handle)
	{
		const uint32 TargetIndex = this->ResolveDenseIndex(Handle);
		const uint32 LastIndex = this->DenseCount - 1;
		T *Target = this->Storage + TargetIndex;
		T *Last = this->Storage + LastIndex;
		Relocation Relocation;

		Target->~T();
		if (TargetIndex != LastIndex)
		{
			const uint32 MovedSlot = this->DenseToSlot[LastIndex];
			::new (static_cast<void *>(Target)) T(std::move(*Last));
			Last->~T();
			this->DenseToSlot[TargetIndex] = MovedSlot;
			this->SlotToDense[MovedSlot] = TargetIndex;
			Relocation = {
				.OldAddress = Last, .NewAddress = Target, .Handle = {.Slot = MovedSlot, .Generation = this->Generations[MovedSlot]}};
		}

		this->DenseToSlot[LastIndex] = InvalidSceneSlot;
		this->SlotToDense[Handle.Slot] = InvalidSceneSlot;
		--this->DenseCount;
		++this->Generations[Handle.Slot];
		if (this->Generations[Handle.Slot] == 0)
			++this->Generations[Handle.Slot];
		this->FreeSlots.push_back(Handle.Slot);
		return Relocation;
	}

	[[nodiscard]] T *TryResolve(DensePoolHandle Handle) noexcept
	{
		if (!this->Contains(Handle))
			return nullptr;
		return this->Storage + this->SlotToDense[Handle.Slot];
	}

	[[nodiscard]] const T *TryResolve(DensePoolHandle Handle) const noexcept
	{
		if (!this->Contains(Handle))
			return nullptr;
		return this->Storage + this->SlotToDense[Handle.Slot];
	}

	[[nodiscard]] bool Contains(DensePoolHandle Handle) const noexcept
	{
		return Handle.Slot < this->CapacityValue && Handle.Generation != 0 && this->Generations[Handle.Slot] == Handle.Generation &&
			   this->SlotToDense[Handle.Slot] != InvalidSceneSlot;
	}

	[[nodiscard]] DensePoolHandle HandleAtDense(uint32 DenseIndex) const
	{
		if (DenseIndex >= this->DenseCount)
			throw std::out_of_range("Dense Scene pool index is out of range");
		const uint32 Slot = this->DenseToSlot[DenseIndex];
		return {.Slot = Slot, .Generation = this->Generations[Slot]};
	}

	[[nodiscard]] std::span<T> Span() noexcept
	{
		return {this->Storage, this->DenseCount};
	}
	[[nodiscard]] std::span<const T> Span() const noexcept
	{
		return {this->Storage, this->DenseCount};
	}
	[[nodiscard]] uint32 Size() const noexcept
	{
		return this->DenseCount;
	}
	[[nodiscard]] uint32 Capacity() const noexcept
	{
		return this->CapacityValue;
	}

	void Clear() noexcept
	{
		while (this->DenseCount != 0)
		{
			--this->DenseCount;
			(this->Storage + this->DenseCount)->~T();
		}
	}

  private:
	T *Storage = nullptr;
	uint32 CapacityValue = 0;
	uint32 DenseCount = 0;
	uint32 NextUnusedSlot = 0;
	usize PageSize = 0;
	usize ReservedBytes = 0;
	usize CommittedBytes = 0;
	std::vector<uint32> SlotToDense;
	std::vector<uint32> DenseToSlot;
	std::vector<uint32> Generations;
	std::vector<uint32> FreeSlots;

	[[nodiscard]] uint32 ResolveDenseIndex(DensePoolHandle Handle) const
	{
		if (!this->Contains(Handle))
			throw std::out_of_range("Dense Scene pool handle is stale or invalid");
		return this->SlotToDense[Handle.Slot];
	}

	void CommitFor(uint32 ElementCount)
	{
		const usize RequestedBytes = static_cast<usize>(ElementCount) * sizeof(T);
		const usize AlignedBytes = ((RequestedBytes + this->PageSize - 1) / this->PageSize) * this->PageSize;
		if (AlignedBytes <= this->CommittedBytes)
			return;
		if (AlignedBytes > this->ReservedBytes + this->PageSize - 1)
		{
			throw SceneCapacityException("Scene pool commit exceeds reserved address space");
		}
		void *Committed = VirtualAlloc(reinterpret_cast<uint8 *>(this->Storage) + this->CommittedBytes, AlignedBytes - this->CommittedBytes,
									   MEM_COMMIT, PAGE_READWRITE);
		if (Committed == nullptr)
		{
			throw SceneCapacityException("Failed to commit Scene pool pages");
		}
		this->CommittedBytes = AlignedBytes;
	}
};
} // namespace world::detail
