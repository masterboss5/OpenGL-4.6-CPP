#pragma once

#include "PickTable.h"
#include "src/types.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

namespace pipeline::render
{
inline constexpr uint32 MaximumPickObjectCount = 65'536;
inline constexpr uint32 SelectionMaskBitsPerWord = sizeof(uint32) * 8U;
inline constexpr uint32 MaximumSelectionMaskWordCount =
	(MaximumPickObjectCount + 1U + SelectionMaskBitsPerWord - 1U) / SelectionMaskBitsPerWord;
static_assert(MaximumSelectionMaskWordCount == 2'049U);

class ENGINE_API SelectionMask final
{
  public:
	SelectionMask()
	{
		this->Words.reserve(MaximumSelectionMaskWordCount);
	}

	[[nodiscard]] static SelectionMask Build(const FramePickTable &Table, const std::span<const world::ObjectHandle> SelectedObjects)
	{
		SelectionMask Result;
		Result.BuildInto(Table, SelectedObjects);
		return Result;
	}

	void BuildInto(const FramePickTable &Table, const std::span<const world::ObjectHandle> SelectedObjects)
	{
		if (Table.GetObjectCount() > MaximumPickObjectCount)
			throw std::overflow_error("The frame pick table exceeds the selection-mask capacity");

		const uint32 HighestIdentifier = static_cast<uint32>(Table.GetObjectCount());
		const uint32 WordCount = std::max(1U, HighestIdentifier / SelectionMaskBitsPerWord + 1U);
		this->Words.assign(WordCount, 0U);
		for (const world::ObjectHandle Object : SelectedObjects)
		{
			const std::optional<PickID> Identifier = Table.Find(Object);
			if (!Identifier.has_value())
				continue;
			const uint32 Word = *Identifier / SelectionMaskBitsPerWord;
			const uint32 Bit = *Identifier % SelectionMaskBitsPerWord;
			this->Words[Word] |= 1U << Bit;
		}
	}

	[[nodiscard]] bool Contains(const PickID Identifier) const noexcept
	{
		const uint32 Word = Identifier / SelectionMaskBitsPerWord;
		const uint32 Bit = Identifier % SelectionMaskBitsPerWord;
		return Word < this->Words.size() && (this->Words[Word] & (1U << Bit)) != 0U;
	}

	[[nodiscard]] const std::vector<uint32> &GetWords() const noexcept
	{
		return this->Words;
	}

	[[nodiscard]] uint32 GetWordCount() const noexcept
	{
		return static_cast<uint32>(this->Words.size());
	}

	void Clear() noexcept
	{
		this->Words.clear();
	}

  private:
	std::vector<uint32> Words;
};
} // namespace pipeline::render
