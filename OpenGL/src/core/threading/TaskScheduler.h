#pragma once

#include "src/concepts.h"
#include "TaskScratchAllocator.h"
#include "src/types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace core::threading
{
enum class TaskPriority : uint8
{
	Background,
	Normal,
	Critical,
	Count
};

struct TaskSchedulerSpecification final
{
	uint32 WorkerCount = 0;
	usize Capacity = 65'536;
	usize ScratchCapacity = 1U << 20U;
};

class ENGINE_API TaskSchedulerStoppedException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API TaskSchedulerWaitException final : public std::logic_error
{
  public:
	using std::logic_error::logic_error;
};

class ENGINE_API TaskScheduler final
{
  public:
	using Task = std::function<void()>;

	explicit TaskScheduler(TaskSchedulerSpecification Specification = {});
	~TaskScheduler();

	TaskScheduler(const TaskScheduler &) = delete;
	TaskScheduler &operator=(const TaskScheduler &) = delete;
	TaskScheduler(TaskScheduler &&) = delete;
	TaskScheduler &operator=(TaskScheduler &&) = delete;

	void Schedule(Task Work, TaskPriority Priority = TaskPriority::Normal);
	[[nodiscard]] bool TrySchedule(Task Work, TaskPriority Priority = TaskPriority::Normal);

	template <TaskCallable Callable>
	[[nodiscard]] auto Submit(Callable &&Work, const TaskPriority Priority = TaskPriority::Normal)
		-> std::future<std::invoke_result_t<std::remove_reference_t<Callable> &>>
	{
		using Result = std::invoke_result_t<std::remove_reference_t<Callable> &>;
		auto WorkPackage = std::make_shared<std::packaged_task<Result()>>(std::forward<Callable>(Work));
		std::future<Result> Future = WorkPackage->get_future();
		this->Schedule([WorkPackage]() mutable { (*WorkPackage)(); }, Priority);
		return Future;
	}

	void WaitIdle();
	[[nodiscard]] uint32 GetWorkerCount() const noexcept;
	[[nodiscard]] usize GetCapacity() const noexcept;
	[[nodiscard]] usize GetPendingTaskCount() const noexcept;
	[[nodiscard]] bool IsWorkerThread() const noexcept;
	[[nodiscard]] static bool IsCurrentThreadWorker() noexcept;
	[[nodiscard]] TaskScratchAllocator &GetCurrentWorkerScratch();

  private:
	struct WorkerQueue final
	{
		std::mutex Mutex;
		std::deque<Task> Tasks[static_cast<usize>(TaskPriority::Count)];
	};

	void WorkerMain(uint32 WorkerIndex, std::stop_token StopToken);
	[[nodiscard]] bool TryTakeLocal(uint32 WorkerIndex, Task &Work);
	[[nodiscard]] bool TrySteal(uint32 WorkerIndex, Task &Work);
	[[nodiscard]] bool TryTakeExternal(Task &Work);
	[[nodiscard]] bool HasAvailableWork() const;
	void CompleteTask() noexcept;
	[[nodiscard]] static usize PriorityIndex(TaskPriority Priority) noexcept;

	TaskSchedulerSpecification Specification;
	std::vector<std::unique_ptr<WorkerQueue>> WorkerQueues;
	std::vector<std::unique_ptr<TaskScratchAllocator>> ScratchAllocators;
	std::vector<std::jthread> Workers;
	mutable std::mutex ExternalMutex;
	std::deque<Task> ExternalTasks[static_cast<usize>(TaskPriority::Count)];
	std::condition_variable_any WorkCondition;
	std::condition_variable CapacityCondition;
	std::condition_variable IdleCondition;
	std::atomic<usize> PendingTaskCount = 0;
	std::atomic<usize> QueuedTaskCount = 0;
	std::atomic<bool> AcceptingTasks = true;
};

class ENGINE_API TaskGroup final
{
  public:
	TaskGroup();

	TaskGroup(const TaskGroup &) = delete;
	TaskGroup &operator=(const TaskGroup &) = delete;
	TaskGroup(TaskGroup &&) noexcept = default;
	TaskGroup &operator=(TaskGroup &&) noexcept = default;

	template <TaskCallable Callable> void Run(TaskScheduler &Scheduler, Callable &&Work, const TaskPriority Priority = TaskPriority::Normal)
	{
		std::shared_ptr<State> SharedState = this->SharedState;
		SharedState->Pending.fetch_add(1, std::memory_order_relaxed);
		try
		{
			Scheduler.Schedule(
				[SharedState, Work = std::forward<Callable>(Work)]() mutable
				{
					const State *PreviousState = TaskGroup::SetCurrentExecutingState(SharedState.get());
					try
					{
						std::invoke(Work);
					}
					catch (...)
					{
						std::scoped_lock Lock(SharedState->Mutex);
						if (SharedState->Failure == nullptr)
							SharedState->Failure = std::current_exception();
					}
					(void)TaskGroup::SetCurrentExecutingState(PreviousState);

					if (SharedState->Pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
						SharedState->Condition.notify_all();
				},
				Priority);
		}
		catch (...)
		{
			if (SharedState->Pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
				SharedState->Condition.notify_all();
			throw;
		}
	}

	void Wait();
	[[nodiscard]] bool IsComplete() const noexcept;

  private:
	struct State final
	{
		std::atomic<usize> Pending = 0;
		std::mutex Mutex;
		std::condition_variable Condition;
		std::exception_ptr Failure;
	};

	[[nodiscard]] static const State *SetCurrentExecutingState(const State *State) noexcept;

	std::shared_ptr<State> SharedState;
};
} // namespace core::threading
