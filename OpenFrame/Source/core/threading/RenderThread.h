#pragma once

#include "Source/concepts.h"
#include "Source/types.h"

#include <atomic>
#include <condition_variable>
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

namespace core
{
class Context;
}

namespace core::threading
{
struct RenderThreadSpecification final
{
	usize QueueCapacity = 4'096;
};

class ENGINE_API RenderThreadStoppedException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API RenderThread final
{
  public:
	using Command = std::function<void()>;
	using CommandFailure = std::function<void(std::exception_ptr)>;

	explicit RenderThread(RenderThreadSpecification Specification = {});
	~RenderThread();

	RenderThread(const RenderThread &) = delete;
	RenderThread &operator=(const RenderThread &) = delete;
	RenderThread(RenderThread &&) = delete;
	RenderThread &operator=(RenderThread &&) = delete;

	void Start(Context &Context);
	void Stop();
	void Flush();

	template <TaskCallable Callable>
	[[nodiscard]] auto Submit(Callable &&Work) -> std::future<std::invoke_result_t<std::remove_reference_t<Callable> &>>
	{
		using Result = std::invoke_result_t<std::remove_reference_t<Callable> &>;
		auto Promise = std::make_shared<std::promise<Result>>();
		std::future<Result> Future = Promise->get_future();
		this->Enqueue(QueuedCommand{.Execute =
										[Promise, Work = std::forward<Callable>(Work)]() mutable
									{
										try
										{
											if constexpr (std::is_void_v<Result>)
											{
												std::invoke(Work);
												Promise->set_value();
											}
											else
												Promise->set_value(std::invoke(Work));
										}
										catch (...)
										{
											const std::exception_ptr Failure = std::current_exception();
											try
											{
												Promise->set_exception(Failure);
											}
											catch (...)
											{
											}
											std::rethrow_exception(Failure);
										}
									},
									.Fail =
										[Promise](const std::exception_ptr Failure)
									{
										try
										{
											Promise->set_exception(Failure != nullptr
																	   ? Failure
																	   : std::make_exception_ptr(RenderThreadStoppedException(
																			 "RenderThread command was abandoned")));
										}
										catch (...)
										{
										}
									}});
		return Future;
	}

	[[nodiscard]] bool TryEnqueue(Command Work);
	// The failure callback runs on the render thread if the command was queued
	// but abandoned after a worker failure. It closes frame-owned completion
	// state so owner-thread callers cannot wait forever on a lost packet.
	[[nodiscard]] bool TryEnqueue(Command Work, CommandFailure Failure);
	[[nodiscard]] bool IsRunning() const noexcept;
	[[nodiscard]] bool IsRenderThread() const;
	[[nodiscard]] usize GetQueueCapacity() const noexcept;
	[[nodiscard]] usize GetQueuedCommandCount() const;

  private:
	struct QueuedCommand final
	{
		Command Execute;
		std::function<void(std::exception_ptr)> Fail;
	};

	void Enqueue(Command Work);
	void Enqueue(QueuedCommand Work);
	[[nodiscard]] bool TryEnqueue(QueuedCommand Work);
	void ThreadMain(std::stop_token StopToken, std::shared_ptr<std::promise<void>> Ready);
	void RethrowThreadFailure();
	void FailQueuedCommands(std::exception_ptr Failure) noexcept;

	RenderThreadSpecification Specification;
	Context *BoundContext = nullptr;
	std::jthread Worker;
	std::thread::id WorkerID;
	mutable std::mutex Mutex;
	std::vector<QueuedCommand> CommandRing;
	usize CommandReadIndex = 0;
	usize CommandWriteIndex = 0;
	usize CommandCount = 0;
	std::condition_variable_any WorkCondition;
	std::condition_variable CapacityCondition;
	std::atomic<bool> Running = false;
	bool AcceptingCommands = false;
	std::exception_ptr ThreadFailure;
};
} // namespace core::threading
