#include "RenderThread.h"

#include "src/core/window/Context.h"

namespace core::threading
{
RenderThread::RenderThread(RenderThreadSpecification Specification) : Specification(Specification)
{
	if (this->Specification.QueueCapacity == 0)
		throw std::invalid_argument("RenderThread queue capacity must be non-zero");
	this->CommandRing.resize(this->Specification.QueueCapacity);
}

RenderThread::~RenderThread()
{
	try
	{
		this->Stop();
	}
	catch (...)
	{
		// Explicit Stop() is the reporting boundary for render-thread failures.
	}
}

void RenderThread::Start(Context &Context)
{
	{
		std::scoped_lock Lock(this->Mutex);
		if (this->Running.load(std::memory_order_acquire) || this->Worker.joinable())
			throw std::logic_error("RenderThread is already running");
		if (this->CommandCount != 0)
			throw std::logic_error("RenderThread cannot start with queued commands");
		this->BoundContext = &Context;
		this->ThreadFailure = nullptr;
		this->AcceptingCommands = false;
	}

	try
	{
		Context.PrepareThreadTransfer();
	}
	catch (...)
	{
		std::scoped_lock Lock(this->Mutex);
		this->AcceptingCommands = false;
		this->BoundContext = nullptr;
		throw;
	}
	auto Ready = std::make_shared<std::promise<void>>();
	std::future<void> ReadyFuture = Ready->get_future();
	try
	{
		this->Worker = std::jthread([this, Ready](const std::stop_token StopToken) { this->ThreadMain(StopToken, Ready); });
		ReadyFuture.get();
	}
	catch (...)
	{
		{
			std::scoped_lock Lock(this->Mutex);
			this->AcceptingCommands = false;
		}
		if (this->Worker.joinable())
		{
			this->Worker.request_stop();
			this->WorkCondition.notify_all();
			this->Worker.join();
		}
		if (Context.IsThreadTransferPending())
			Context.AdoptCurrentThread();
		{
			std::scoped_lock Lock(this->Mutex);
			this->BoundContext = nullptr;
		}
		throw;
	}
}

void RenderThread::Stop()
{
	if (!this->Worker.joinable())
		return;
	if (this->IsRenderThread())
		throw std::logic_error("RenderThread cannot stop or join itself");

	std::exception_ptr StopFailure;
	try
	{
		this->Flush();
	}
	catch (...)
	{
		StopFailure = std::current_exception();
	}
	{
		std::scoped_lock Lock(this->Mutex);
		this->AcceptingCommands = false;
	}
	this->Worker.request_stop();
	this->WorkCondition.notify_all();
	this->CapacityCondition.notify_all();
	this->Worker.join();

	Context *Context = this->BoundContext;
	this->BoundContext = nullptr;
	{
		std::scoped_lock Lock(this->Mutex);
		while (this->CommandCount != 0)
		{
			this->CommandRing[this->CommandReadIndex] = {};
			this->CommandReadIndex = (this->CommandReadIndex + 1U) % this->CommandRing.size();
			--this->CommandCount;
		}
		this->CommandReadIndex = 0;
		this->CommandWriteIndex = 0;
	}
	if (Context != nullptr && Context->IsThreadTransferPending())
		Context->AdoptCurrentThread();
	std::exception_ptr WorkerFailure;
	try
	{
		this->RethrowThreadFailure();
	}
	catch (...)
	{
		WorkerFailure = std::current_exception();
	}
	if (WorkerFailure != nullptr)
		std::rethrow_exception(WorkerFailure);
	if (StopFailure != nullptr)
		std::rethrow_exception(StopFailure);
}

void RenderThread::Flush()
{
	if (!this->Worker.joinable())
		return;
	if (this->IsRenderThread())
		throw std::logic_error("RenderThread cannot synchronously flush itself");
	this->Submit([]() {}).get();
	this->RethrowThreadFailure();
}

bool RenderThread::TryEnqueue(Command Work)
{
	if (!Work)
		throw std::invalid_argument("RenderThread cannot enqueue an empty command");
	return this->TryEnqueue(QueuedCommand{.Execute = std::move(Work)});
}

bool RenderThread::TryEnqueue(Command Work, CommandFailure Failure)
{
	if (!Work)
		throw std::invalid_argument("RenderThread cannot enqueue an empty command");
	if (!Failure)
		return this->TryEnqueue(std::move(Work));
	return this->TryEnqueue(QueuedCommand{.Execute = std::move(Work), .Fail = std::move(Failure)});
}

bool RenderThread::TryEnqueue(QueuedCommand Work)
{
	if (!Work.Execute)
		throw std::invalid_argument("RenderThread cannot enqueue an empty command");
	std::unique_lock Lock(this->Mutex, std::try_to_lock);
	if (!Lock.owns_lock() || !this->AcceptingCommands || this->CommandCount >= this->Specification.QueueCapacity)
		return false;
	this->CommandRing[this->CommandWriteIndex] = std::move(Work);
	this->CommandWriteIndex = (this->CommandWriteIndex + 1U) % this->CommandRing.size();
	++this->CommandCount;
	Lock.unlock();
	this->WorkCondition.notify_one();
	return true;
}

bool RenderThread::IsRunning() const noexcept
{
	return this->Running.load(std::memory_order_acquire);
}

bool RenderThread::IsRenderThread() const
{
	std::scoped_lock Lock(this->Mutex);
	return this->WorkerID == std::this_thread::get_id();
}

usize RenderThread::GetQueueCapacity() const noexcept
{
	return this->Specification.QueueCapacity;
}

usize RenderThread::GetQueuedCommandCount() const
{
	std::scoped_lock Lock(this->Mutex);
	return this->CommandCount;
}

void RenderThread::Enqueue(Command Work)
{
	if (!Work)
		throw std::invalid_argument("RenderThread cannot enqueue an empty command");
	this->Enqueue(QueuedCommand{.Execute = std::move(Work)});
}

void RenderThread::Enqueue(QueuedCommand Work)
{
	if (!Work.Execute)
		throw std::invalid_argument("RenderThread cannot enqueue an empty command");
	std::unique_lock Lock(this->Mutex);
	this->CapacityCondition.wait(Lock,
								 [this]() { return !this->AcceptingCommands || this->CommandCount < this->Specification.QueueCapacity; });
	if (!this->AcceptingCommands)
		throw RenderThreadStoppedException("RenderThread is not accepting commands");
	this->CommandRing[this->CommandWriteIndex] = std::move(Work);
	this->CommandWriteIndex = (this->CommandWriteIndex + 1U) % this->CommandRing.size();
	++this->CommandCount;
	Lock.unlock();
	this->WorkCondition.notify_one();
}

void RenderThread::ThreadMain(const std::stop_token StopToken, const std::shared_ptr<std::promise<void>> Ready)
{
	{
		std::scoped_lock Lock(this->Mutex);
		this->WorkerID = std::this_thread::get_id();
	}
	try
	{
		this->BoundContext->AdoptCurrentThread();
		{
			std::scoped_lock Lock(this->Mutex);
			this->AcceptingCommands = true;
		}
		this->Running.store(true, std::memory_order_release);
		Ready->set_value();

		while (true)
		{
			QueuedCommand Work;
			{
				std::unique_lock Lock(this->Mutex);
				this->WorkCondition.wait(Lock, StopToken,
										 [this, &StopToken]() { return StopToken.stop_requested() || this->CommandCount != 0; });
				if (this->CommandCount == 0)
				{
					if (StopToken.stop_requested())
						break;
					continue;
				}
				Work = std::move(this->CommandRing[this->CommandReadIndex]);
				this->CommandRing[this->CommandReadIndex] = {};
				this->CommandReadIndex = (this->CommandReadIndex + 1U) % this->CommandRing.size();
				--this->CommandCount;
			}
			this->CapacityCondition.notify_one();
			Work.Execute();
		}

		this->BoundContext->PrepareThreadTransfer();
		this->Running.store(false, std::memory_order_release);
		{
			std::scoped_lock Lock(this->Mutex);
			this->AcceptingCommands = false;
			this->WorkerID = {};
		}
		this->CapacityCondition.notify_all();
	}
	catch (...)
	{
		const std::exception_ptr Failure = std::current_exception();
		try
		{
			if (this->BoundContext != nullptr && this->BoundContext->IsCurrent())
				this->BoundContext->PrepareThreadTransfer();
		}
		catch (...)
		{
		}
		{
			std::scoped_lock Lock(this->Mutex);
			this->ThreadFailure = Failure;
			this->AcceptingCommands = false;
			this->WorkerID = {};
		}
		this->FailQueuedCommands(Failure);
		this->Running.store(false, std::memory_order_release);
		this->CapacityCondition.notify_all();
		try
		{
			Ready->set_exception(Failure);
		}
		catch (...)
		{
		}
	}
}

void RenderThread::FailQueuedCommands(const std::exception_ptr Failure) noexcept
{
	usize PendingCount = 0;
	usize ReadIndex = 0;
	{
		std::scoped_lock Lock(this->Mutex);
		PendingCount = this->CommandCount;
		ReadIndex = this->CommandReadIndex;
		this->CommandCount = 0;
		this->CommandReadIndex = 0;
		this->CommandWriteIndex = 0;
	}
	this->CapacityCondition.notify_all();
	for (usize Index = 0; Index < PendingCount; ++Index)
	{
		QueuedCommand Command = std::move(this->CommandRing[ReadIndex]);
		this->CommandRing[ReadIndex] = {};
		ReadIndex = (ReadIndex + 1U) % this->CommandRing.size();
		if (!Command.Fail)
			continue;
		try
		{
			Command.Fail(Failure);
		}
		catch (...)
		{
		}
	}
}

void RenderThread::RethrowThreadFailure()
{
	std::exception_ptr Failure;
	{
		std::scoped_lock Lock(this->Mutex);
		Failure = this->ThreadFailure;
		this->ThreadFailure = nullptr;
	}
	if (Failure != nullptr)
		std::rethrow_exception(Failure);
}
} // namespace core::threading
