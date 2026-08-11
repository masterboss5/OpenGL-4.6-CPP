#include "TaskScratchAllocator.h"

#include <memory>
#include <new>
#include <stdexcept>

namespace core::threading
{
TaskScratchAllocator::TaskScratchAllocator(const usize Capacity) : Storage(Capacity)
{
	if (Capacity == 0)
		throw std::invalid_argument("Task scratch allocator capacity must be non-zero");
}

void *TaskScratchAllocator::Allocate(const usize Size, const usize Alignment)
{
	if (Size == 0)
		return nullptr;
	if (Alignment == 0 || (Alignment & (Alignment - 1U)) != 0)
		throw std::invalid_argument("Task scratch allocation alignment must be a non-zero power of two");
	void *Address = this->Storage.data() + this->Offset;
	usize Available = this->Storage.size() - this->Offset;
	if (std::align(Alignment, Size, Address, Available) == nullptr)
		throw std::bad_alloc();
	this->Offset = this->Storage.size() - Available + Size;
	return Address;
}

void TaskScratchAllocator::Reset() noexcept
{
	this->Offset = 0;
}

usize TaskScratchAllocator::GetCapacity() const noexcept
{
	return this->Storage.size();
}

usize TaskScratchAllocator::GetUsed() const noexcept
{
	return this->Offset;
}
} // namespace core::threading
