#pragma once

#include "Source/scene/SceneHandles.h"
#include "Source/types.h"

#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

class SceneCollection;

namespace pipeline::render
{
using PickID = uint32;

inline constexpr PickID BackgroundPickID = 0;

struct ObjectHandleHash final
{
	[[nodiscard]] usize operator()(const world::ObjectHandle &Handle) const noexcept
	{
		uint64 Hash = 1469598103934665603ULL;
		const auto Mix = [&Hash](const uint64 Value)
		{
			Hash ^= Value;
			Hash *= 1099511628211ULL;
		};
		Mix(Handle.Scene);
		Mix(Handle.Slot);
		Mix(Handle.Generation);
		return static_cast<usize>(Hash);
	}
};

class ENGINE_API FramePickTable final
{
  public:
	explicit FramePickTable(const uint64 FrameNumber, const uint32 ExpectedObjectCount = 256)
		: FrameNumber(FrameNumber), Capacity(ExpectedObjectCount)
	{
		if (ExpectedObjectCount == 0 || ExpectedObjectCount >= std::numeric_limits<PickID>::max())
			throw std::invalid_argument("Frame pick-table capacity must be non-zero and representable by PickID");
		this->Objects.reserve(ExpectedObjectCount + 1U);
		this->Identifiers.reserve(ExpectedObjectCount);
		this->Objects.emplace_back();
	}

	[[nodiscard]] PickID Register(const world::ObjectHandle Object)
	{
		if (!Object.IsValid())
			throw std::invalid_argument("A frame pick table cannot register an invalid object handle");

		const auto Existing = this->Identifiers.find(Object);
		if (Existing != this->Identifiers.end() && Existing->second.Generation == this->IdentifierGeneration)
			return Existing->second.Identifier;
		if (this->Objects.size() - 1U >= this->Capacity)
			throw std::overflow_error("Frame pick-table capacity exceeded");
		if (this->Objects.size() > std::numeric_limits<PickID>::max())
			throw std::overflow_error("The frame contains more pickable objects than PickID can represent");

		const PickID Identifier = static_cast<PickID>(this->Objects.size());
		this->Objects.push_back(Object);
		this->Identifiers.insert_or_assign(Object, IdentifierEntry{.Identifier = Identifier, .Generation = this->IdentifierGeneration});
		return Identifier;
	}

	[[nodiscard]] std::optional<world::ObjectHandle> Resolve(const PickID Identifier) const noexcept
	{
		if (Identifier == BackgroundPickID || Identifier >= this->Objects.size())
			return std::nullopt;
		return this->Objects[Identifier];
	}

	[[nodiscard]] std::optional<PickID> Find(const world::ObjectHandle Object) const noexcept
	{
		const auto Identifier = this->Identifiers.find(Object);
		if (Identifier == this->Identifiers.end() || Identifier->second.Generation != this->IdentifierGeneration)
			return std::nullopt;
		return Identifier->second.Identifier;
	}

	[[nodiscard]] uint64 GetFrameNumber() const noexcept
	{
		return this->FrameNumber;
	}

	[[nodiscard]] usize GetObjectCount() const noexcept
	{
		return this->Objects.size() - 1;
	}

  private:
	friend class ::SceneCollection;

	void Reset(const uint64 NewFrameNumber)
	{
		this->FrameNumber = NewFrameNumber;
		this->Objects.clear();
		++this->IdentifierGeneration;
		if (this->IdentifierGeneration == 0)
		{
			this->Identifiers.clear();
			this->IdentifierGeneration = 1;
		}
		if (this->Identifiers.size() > static_cast<usize>(this->Capacity) * 4U)
		{
			const uint64 PublishedGeneration = this->IdentifierGeneration;
			std::erase_if(this->Identifiers,
						  [PublishedGeneration](const auto &Entry) { return Entry.second.Generation != PublishedGeneration; });
		}
		this->Objects.emplace_back();
	}

	struct IdentifierEntry final
	{
		PickID Identifier = BackgroundPickID;
		uint64 Generation = 0;
	};

	uint64 FrameNumber = 0;
	uint32 Capacity = 0;
	std::vector<world::ObjectHandle> Objects;
	std::unordered_map<world::ObjectHandle, IdentifierEntry, ObjectHandleHash> Identifiers;
	uint64 IdentifierGeneration = 0;
};
} // namespace pipeline::render
