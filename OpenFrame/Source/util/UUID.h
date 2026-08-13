#pragma once

#include "Source/types.h"

#include <charconv>
#include <compare>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <stdexcept>

namespace util
{
class UUID final
{
  public:
	constexpr UUID() noexcept = default;
	constexpr UUID(const uint64 Left, const uint64 Right) noexcept : Left(Left), Right(Right)
	{
	}

	[[nodiscard]] static UUID GenerateRandomUUID()
	{
		thread_local std::mt19937_64 Generator{std::random_device{}()};
		thread_local std::uniform_int_distribution<uint64> Distribution;
		uint64 Left = Distribution(Generator);
		uint64 Right = Distribution(Generator);
		Left = (Left & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
		Right = (Right & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
		return UUID(Left, Right);
	}

	[[nodiscard]] static std::optional<UUID> TryParse(const string_view Text) noexcept
	{
		if (Text.size() != 36 || Text[8] != '-' || Text[13] != '-' || Text[18] != '-' || Text[23] != '-')
			return std::nullopt;

		string Digits;
		Digits.reserve(32);
		for (const auto Character : Text)
		{
			if (Character != '-')
				Digits.push_back(Character);
		}
		if (Digits.size() != 32)
			return std::nullopt;

		uint64 Left = 0;
		uint64 Right = 0;
		const auto LeftResult = std::from_chars(Digits.data(), Digits.data() + 16, Left, 16);
		const auto RightResult = std::from_chars(Digits.data() + 16, Digits.data() + 32, Right, 16);
		if (LeftResult.ec != std::errc{} || LeftResult.ptr != Digits.data() + 16 || RightResult.ec != std::errc{} ||
			RightResult.ptr != Digits.data() + 32)
		{
			return std::nullopt;
		}
		return UUID(Left, Right);
	}

	[[nodiscard]] static UUID Parse(const string_view Text)
	{
		const std::optional<UUID> Result = UUID::TryParse(Text);
		if (!Result.has_value())
			throw std::invalid_argument("UUID text must use the canonical 8-4-4-4-12 hexadecimal form");
		return *Result;
	}

	[[nodiscard]] constexpr bool IsValid() const noexcept
	{
		return this->Left != 0 || this->Right != 0;
	}

	[[nodiscard]] constexpr explicit operator bool() const noexcept
	{
		return this->IsValid();
	}

	[[nodiscard]] string ToString() const
	{
		return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", static_cast<uint32>(this->Left >> 32),
						   static_cast<uint16>(this->Left >> 16), static_cast<uint16>(this->Left), static_cast<uint16>(this->Right >> 48),
						   this->Right & 0x0000FFFFFFFFFFFFULL);
	}

	[[nodiscard]] constexpr uint64 GetLeft() const noexcept
	{
		return this->Left;
	}

	[[nodiscard]] constexpr uint64 GetRight() const noexcept
	{
		return this->Right;
	}

	[[nodiscard]] constexpr auto operator<=>(const UUID &) const noexcept = default;

  private:
	uint64 Left = 0;
	uint64 Right = 0;

	friend std::hash<UUID>;
};
} // namespace util

template <> struct std::hash<util::UUID>
{
	[[nodiscard]] usize operator()(const util::UUID &ID) const noexcept
	{
		const usize LeftHash = std::hash<uint64>{}(ID.Left);
		const usize RightHash = std::hash<uint64>{}(ID.Right);
		return LeftHash ^ (RightHash + static_cast<usize>(0x9E3779B97F4A7C15ULL) + (LeftHash << 6) + (LeftHash >> 2));
	}
};
