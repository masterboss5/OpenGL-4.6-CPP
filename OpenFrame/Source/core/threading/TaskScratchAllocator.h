#pragma once

#include "Source/core/EngineAPI.h"
#include "Source/types.h"

#include <cstddef>
#include <vector>

namespace core::threading
{
class ENGINE_API TaskScratchAllocator final
{
  public:
	explicit TaskScratchAllocator(usize Capacity = 1U << 20U);
	~TaskScratchAllocator() = default;

	TaskScratchAllocator(const TaskScratchAllocator &) = delete;
	TaskScratchAllocator &operator=(const TaskScratchAllocator &) = delete;
	TaskScratchAllocator(TaskScratchAllocator &&) = delete;
	TaskScratchAllocator &operator=(TaskScratchAllocator &&) = delete;

	[[nodiscard]] void *Allocate(usize Size, usize Alignment = alignof(std::max_align_t));
	void Reset() noexcept;
	[[nodiscard]] usize GetCapacity() const noexcept;
	[[nodiscard]] usize GetUsed() const noexcept;

  private:
	std::vector<std::byte> Storage;
	usize Offset = 0;
};
} // namespace core::threading
