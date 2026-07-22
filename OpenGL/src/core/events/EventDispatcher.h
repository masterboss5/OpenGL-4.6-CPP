#pragma once

#include "src/types.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace core
{
enum class EventPropagation : uint8
{
	Continue,
	Consumed
};

class EventSubscription final
{
  public:
	EventSubscription() = default;
	explicit EventSubscription(std::function<void()> Unsubscribe) : Unsubscribe(std::move(Unsubscribe))
	{
	}
	~EventSubscription()
	{
		this->Reset();
	}

	EventSubscription(const EventSubscription &) = delete;
	EventSubscription &operator=(const EventSubscription &) = delete;
	EventSubscription(EventSubscription &&Other) noexcept : Unsubscribe(std::move(Other.Unsubscribe))
	{
		Other.Unsubscribe = {};
	}
	EventSubscription &operator=(EventSubscription &&Other) noexcept
	{
		if (this != &Other)
		{
			this->Reset();
			this->Unsubscribe = std::move(Other.Unsubscribe);
			Other.Unsubscribe = {};
		}
		return *this;
	}

	void Reset() noexcept
	{
		if (!this->Unsubscribe)
			return;
		auto Callback = std::move(this->Unsubscribe);
		Callback();
	}
	[[nodiscard]] bool IsSubscribed() const noexcept
	{
		return static_cast<bool>(this->Unsubscribe);
	}

  private:
	std::function<void()> Unsubscribe;
};

template <typename Event> class EventDispatcher final
{
  public:
	using Callback = std::function<EventPropagation(const Event &)>;

	EventDispatcher() : State(std::make_shared<DispatcherState>())
	{
	}

	[[nodiscard]] EventSubscription Subscribe(const int32 Priority, Callback Callback)
	{
		if (!Callback)
			throw std::invalid_argument("Event subscription requires a callback");
		const uint64 ID = this->State->NextID++;
		this->State->Listeners.push_back(
			{.ID = ID, .Priority = Priority, .Sequence = this->State->NextSequence++, .Callback = std::move(Callback)});
		std::ranges::sort(this->State->Listeners, [](const Listener &Left, const Listener &Right)
						  { return Left.Priority == Right.Priority ? Left.Sequence < Right.Sequence : Left.Priority > Right.Priority; });
		std::weak_ptr<DispatcherState> WeakState = this->State;
		return EventSubscription(
			[WeakState, ID]() noexcept
			{
				if (const std::shared_ptr<DispatcherState> State = WeakState.lock())
				{
					const auto Removed = std::remove_if(State->Listeners.begin(), State->Listeners.end(),
														[ID](const Listener &Listener) { return Listener.ID == ID; });
					State->Listeners.erase(Removed, State->Listeners.end());
				}
			});
	}

	[[nodiscard]] EventPropagation Dispatch(const Event &Event)
	{
		std::vector<uint64> ListenerIDs;
		ListenerIDs.reserve(this->State->Listeners.size());
		for (const Listener &Listener : this->State->Listeners)
			ListenerIDs.push_back(Listener.ID);
		for (const uint64 ID : ListenerIDs)
		{
			const auto FoundListener = std::find_if(this->State->Listeners.begin(), this->State->Listeners.end(),
													[ID](const Listener &Candidate) { return Candidate.ID == ID; });
			if (FoundListener == this->State->Listeners.end())
				continue;
			Callback StableCallback = FoundListener->Callback;
			try
			{
				if (StableCallback(Event) == EventPropagation::Consumed)
					return EventPropagation::Consumed;
			}
			catch (...)
			{
				if (this->State->PendingException == nullptr)
					this->State->PendingException = std::current_exception();
			}
		}
		return EventPropagation::Continue;
	}

	void RethrowPendingException()
	{
		std::exception_ptr Pending = std::exchange(this->State->PendingException, nullptr);
		if (Pending != nullptr)
			std::rethrow_exception(Pending);
	}

  private:
	struct Listener final
	{
		uint64 ID = 0;
		int32 Priority = 0;
		uint64 Sequence = 0;
		Callback Callback;
	};
	struct DispatcherState final
	{
		std::vector<Listener> Listeners;
		uint64 NextID = 1;
		uint64 NextSequence = 0;
		std::exception_ptr PendingException;
	};
	std::shared_ptr<DispatcherState> State;
};
} // namespace core
