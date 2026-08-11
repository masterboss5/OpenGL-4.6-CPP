#include "SceneCommandBuffer.h"

#include <exception>

namespace world
{
void SceneCommandBuffer::DestroyObject(ObjectHandle Object)
{
	auto Command = std::make_unique<DestroyObjectCommand>(Object);
	std::scoped_lock Lock(this->Mutex);
	this->Commands.push_back(std::move(Command));
}

usize SceneCommandBuffer::Size() const noexcept
{
	std::scoped_lock Lock(this->Mutex);
	return this->Commands.size();
}

bool SceneCommandBuffer::Empty() const noexcept
{
	return this->Size() == 0;
}

void SceneCommandBuffer::Absorb(SceneCommandBuffer &Source)
{
	if (&Source == this)
		return;
	std::scoped_lock Lock(this->Mutex, Source.Mutex);
	this->Commands.reserve(this->Commands.size() + Source.Commands.size());
	for (auto &Command : Source.Commands)
		this->Commands.push_back(std::move(Command));
	Source.Commands.clear();
}

void SceneCommandBuffer::Execute(Scene &Scene)
{
	std::vector<std::unique_ptr<Command>> Pending;
	{
		std::scoped_lock Lock(this->Mutex);
		Pending.swap(this->Commands);
	}
	for (usize CommandIndex = 0; CommandIndex < Pending.size(); ++CommandIndex)
	{
		try
		{
			Pending[CommandIndex]->Execute(Scene);
		}
		catch (...)
		{
			const string Description = Pending[CommandIndex]->Describe();
			{
				std::scoped_lock Lock(this->Mutex);
				std::vector<std::unique_ptr<Command>> Requeued;
				Requeued.reserve((Pending.size() - CommandIndex - 1U) + this->Commands.size());
				for (usize TailIndex = CommandIndex + 1U; TailIndex < Pending.size(); ++TailIndex)
					Requeued.push_back(std::move(Pending[TailIndex]));
				for (auto &ConcurrentCommand : this->Commands)
					Requeued.push_back(std::move(ConcurrentCommand));
				this->Commands.swap(Requeued);
			}
			std::throw_with_nested(SceneCommandExecutionException(CommandIndex, Description));
		}
	}
}

void SceneCommandBuffer::Clear() noexcept
{
	std::scoped_lock Lock(this->Mutex);
	this->Commands.clear();
}
} // namespace world
