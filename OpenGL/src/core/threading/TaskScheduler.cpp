#include "TaskScheduler.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace
{
thread_local const core::threading::TaskScheduler *CurrentScheduler = nullptr;
thread_local uint32 CurrentWorkerIndex = std::numeric_limits<uint32>::max();
thread_local const void *CurrentTaskGroupState = nullptr;
} // namespace

namespace core::threading
{
TaskScheduler::TaskScheduler(TaskSchedulerSpecification Specification) : Specification(Specification)
{
	if (this->Specification.Capacity == 0)
		throw std::invalid_argument("TaskScheduler capacity must be non-zero");
	if (this->Specification.ScratchCapacity == 0)
		throw std::invalid_argument("TaskScheduler scratch capacity must be non-zero");

	if (this->Specification.WorkerCount == 0)
	{
		const uint32 HardwareThreads = std::thread::hardware_concurrency();
		this->Specification.WorkerCount = HardwareThreads > 2 ? HardwareThreads - 2 : 1;
	}

	this->WorkerQueues.reserve(this->Specification.WorkerCount);
	this->ScratchAllocators.reserve(this->Specification.WorkerCount);
	this->Workers.reserve(this->Specification.WorkerCount);
	for (uint32 WorkerIndex = 0; WorkerIndex < this->Specification.WorkerCount; ++WorkerIndex)
	{
		this->WorkerQueues.push_back(std::make_unique<WorkerQueue>());
		this->ScratchAllocators.push_back(std::make_unique<TaskScratchAllocator>(this->Specification.ScratchCapacity));
	}
	for (uint32 WorkerIndex = 0; WorkerIndex < this->Specification.WorkerCount; ++WorkerIndex)
		this->Workers.emplace_back([this, WorkerIndex](const std::stop_token StopToken) { this->WorkerMain(WorkerIndex, StopToken); });
}

TaskScheduler::~TaskScheduler()
{
	this->AcceptingTasks.store(false, std::memory_order_release);
	this->CapacityCondition.notify_all();
	this->WaitIdle();
	for (std::jthread &Worker : this->Workers)
		Worker.request_stop();
	this->WorkCondition.notify_all();
	this->Workers.clear();
}

void TaskScheduler::Schedule(Task Work, const TaskPriority Priority)
{
	if (!Work)
		throw std::invalid_argument("TaskScheduler cannot schedule an empty task");

	if (CurrentScheduler == this && this->PendingTaskCount.load(std::memory_order_acquire) >= this->Specification.Capacity)
	{
		TaskScratchAllocator &Scratch = this->GetCurrentWorkerScratch();
		Scratch.Reset();
		try
		{
			Work();
		}
		catch (...)
		{
			Scratch.Reset();
			throw;
		}
		Scratch.Reset();
		return;
	}

	std::unique_lock Lock(this->ExternalMutex);
	this->CapacityCondition.wait(Lock,
								 [this]()
								 {
									 return !this->AcceptingTasks.load(std::memory_order_acquire) ||
											this->PendingTaskCount.load(std::memory_order_acquire) < this->Specification.Capacity;
								 });
	if (!this->AcceptingTasks.load(std::memory_order_acquire))
		throw TaskSchedulerStoppedException("TaskScheduler is no longer accepting work");

	if (CurrentScheduler == this && CurrentWorkerIndex < this->WorkerQueues.size())
	{
		WorkerQueue &Queue = *this->WorkerQueues[CurrentWorkerIndex];
		{
			std::scoped_lock QueueLock(Queue.Mutex);
			Queue.Tasks[TaskScheduler::PriorityIndex(Priority)].push_back(std::move(Work));
			this->PendingTaskCount.fetch_add(1, std::memory_order_release);
			this->QueuedTaskCount.fetch_add(1, std::memory_order_release);
		}
	}
	else
	{
		this->ExternalTasks[TaskScheduler::PriorityIndex(Priority)].push_back(std::move(Work));
		this->PendingTaskCount.fetch_add(1, std::memory_order_release);
		this->QueuedTaskCount.fetch_add(1, std::memory_order_release);
	}
	Lock.unlock();
	this->WorkCondition.notify_one();
}

bool TaskScheduler::TrySchedule(Task Work, const TaskPriority Priority)
{
	if (!Work)
		throw std::invalid_argument("TaskScheduler cannot schedule an empty task");

	std::unique_lock Lock(this->ExternalMutex, std::try_to_lock);
	if (!Lock.owns_lock() || !this->AcceptingTasks.load(std::memory_order_acquire) ||
		this->PendingTaskCount.load(std::memory_order_acquire) >= this->Specification.Capacity)
	{
		return false;
	}

	if (CurrentScheduler == this && CurrentWorkerIndex < this->WorkerQueues.size())
	{
		WorkerQueue &Queue = *this->WorkerQueues[CurrentWorkerIndex];
		{
			std::scoped_lock QueueLock(Queue.Mutex);
			Queue.Tasks[TaskScheduler::PriorityIndex(Priority)].push_back(std::move(Work));
			this->PendingTaskCount.fetch_add(1, std::memory_order_release);
			this->QueuedTaskCount.fetch_add(1, std::memory_order_release);
		}
	}
	else
	{
		this->ExternalTasks[TaskScheduler::PriorityIndex(Priority)].push_back(std::move(Work));
		this->PendingTaskCount.fetch_add(1, std::memory_order_release);
		this->QueuedTaskCount.fetch_add(1, std::memory_order_release);
	}
	Lock.unlock();
	this->WorkCondition.notify_one();
	return true;
}

void TaskScheduler::WaitIdle()
{
	if (TaskScheduler::IsCurrentThreadWorker())
		throw TaskSchedulerWaitException("A task-scheduler worker cannot wait for scheduler-wide idle state");
	std::unique_lock Lock(this->ExternalMutex);
	this->IdleCondition.wait(Lock, [this]() { return this->PendingTaskCount.load(std::memory_order_acquire) == 0; });
}

uint32 TaskScheduler::GetWorkerCount() const noexcept
{
	return this->Specification.WorkerCount;
}

usize TaskScheduler::GetCapacity() const noexcept
{
	return this->Specification.Capacity;
}

usize TaskScheduler::GetPendingTaskCount() const noexcept
{
	return this->PendingTaskCount.load(std::memory_order_acquire);
}

bool TaskScheduler::IsWorkerThread() const noexcept
{
	return CurrentScheduler == this;
}

bool TaskScheduler::IsCurrentThreadWorker() noexcept
{
	return CurrentScheduler != nullptr;
}

TaskScratchAllocator &TaskScheduler::GetCurrentWorkerScratch()
{
	if (CurrentScheduler != this || CurrentWorkerIndex >= this->ScratchAllocators.size())
		throw std::logic_error("Task scratch storage is only available from a scheduler worker task");
	return *this->ScratchAllocators[CurrentWorkerIndex];
}

void TaskScheduler::WorkerMain(const uint32 WorkerIndex, const std::stop_token StopToken)
{
	CurrentScheduler = this;
	CurrentWorkerIndex = WorkerIndex;

	while (!StopToken.stop_requested())
	{
		Task Work;
		if (this->TryTakeLocal(WorkerIndex, Work) || this->TryTakeExternal(Work) || this->TrySteal(WorkerIndex, Work))
		{
			TaskScratchAllocator &Scratch = *this->ScratchAllocators[WorkerIndex];
			Scratch.Reset();
			try
			{
				Work();
			}
			catch (...)
			{
				// Detached tasks have no receiver. Submit() and TaskGroup preserve exceptions for their callers.
			}
			Scratch.Reset();
			this->CompleteTask();
			continue;
		}

		std::unique_lock Lock(this->ExternalMutex);
		this->WorkCondition.wait_for(Lock, StopToken, std::chrono::milliseconds(2),
									 [this, &StopToken]() { return StopToken.stop_requested() || this->HasAvailableWork(); });
	}

	CurrentWorkerIndex = std::numeric_limits<uint32>::max();
	CurrentScheduler = nullptr;
}

bool TaskScheduler::TryTakeLocal(const uint32 WorkerIndex, Task &Work)
{
	WorkerQueue &Queue = *this->WorkerQueues[WorkerIndex];
	std::scoped_lock Lock(Queue.Mutex);
	for (usize Priority = static_cast<usize>(TaskPriority::Count); Priority-- > 0;)
	{
		if (Queue.Tasks[Priority].empty())
			continue;
		Work = std::move(Queue.Tasks[Priority].back());
		Queue.Tasks[Priority].pop_back();
		this->QueuedTaskCount.fetch_sub(1, std::memory_order_release);
		return true;
	}
	return false;
}

bool TaskScheduler::TrySteal(const uint32 WorkerIndex, Task &Work)
{
	const usize WorkerCount = this->WorkerQueues.size();
	for (usize Offset = 1; Offset < WorkerCount; ++Offset)
	{
		WorkerQueue &Queue = *this->WorkerQueues[(static_cast<usize>(WorkerIndex) + Offset) % WorkerCount];
		std::unique_lock Lock(Queue.Mutex, std::try_to_lock);
		if (!Lock.owns_lock())
			continue;
		for (usize Priority = static_cast<usize>(TaskPriority::Count); Priority-- > 0;)
		{
			if (Queue.Tasks[Priority].empty())
				continue;
			Work = std::move(Queue.Tasks[Priority].front());
			Queue.Tasks[Priority].pop_front();
			this->QueuedTaskCount.fetch_sub(1, std::memory_order_release);
			return true;
		}
	}
	return false;
}

bool TaskScheduler::TryTakeExternal(Task &Work)
{
	std::unique_lock Lock(this->ExternalMutex, std::try_to_lock);
	if (!Lock.owns_lock())
		return false;
	for (usize Priority = static_cast<usize>(TaskPriority::Count); Priority-- > 0;)
	{
		if (this->ExternalTasks[Priority].empty())
			continue;
		Work = std::move(this->ExternalTasks[Priority].front());
		this->ExternalTasks[Priority].pop_front();
		this->QueuedTaskCount.fetch_sub(1, std::memory_order_release);
		return true;
	}
	return false;
}

bool TaskScheduler::HasAvailableWork() const
{
	return this->QueuedTaskCount.load(std::memory_order_acquire) != 0;
}

void TaskScheduler::CompleteTask() noexcept
{
	const usize PreviousCount = this->PendingTaskCount.fetch_sub(1, std::memory_order_acq_rel);
	this->CapacityCondition.notify_one();
	if (PreviousCount == 1)
		this->IdleCondition.notify_all();
}

usize TaskScheduler::PriorityIndex(const TaskPriority Priority) noexcept
{
	return static_cast<usize>(Priority);
}

TaskGroup::TaskGroup() : SharedState(std::make_shared<State>())
{
}

void TaskGroup::Wait()
{
	if (TaskScheduler::IsCurrentThreadWorker())
	{
		if (CurrentTaskGroupState == this->SharedState.get())
			throw TaskSchedulerWaitException("A task cannot wait for its own TaskGroup");
		throw TaskSchedulerWaitException("A task-scheduler worker cannot block on a TaskGroup");
	}
	std::unique_lock Lock(this->SharedState->Mutex);
	this->SharedState->Condition.wait(Lock, [this]() { return this->SharedState->Pending.load(std::memory_order_acquire) == 0; });
	const std::exception_ptr Failure = this->SharedState->Failure;
	this->SharedState->Failure = nullptr;
	Lock.unlock();
	if (Failure != nullptr)
		std::rethrow_exception(Failure);
}

const TaskGroup::State *TaskGroup::SetCurrentExecutingState(const State *ExecutingState) noexcept
{
	const State *PreviousState = static_cast<const State *>(CurrentTaskGroupState);
	CurrentTaskGroupState = ExecutingState;
	return PreviousState;
}

bool TaskGroup::IsComplete() const noexcept
{
	return this->SharedState->Pending.load(std::memory_order_acquire) == 0;
}
} // namespace core::threading
