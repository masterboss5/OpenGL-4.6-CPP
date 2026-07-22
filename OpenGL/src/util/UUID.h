#pragma once
#include "src/types.h"

#include <functional>
#include <random>
#include <string>

namespace util
{
class UUID final
{
  private:
	uint64 Left = 0;
	uint64 Right = 0;

  public:
	UUID() = default;
	UUID(uint64 Left, uint64 Right) : Left(Left), Right(Right)
	{
	}

	[[nodiscard]] inline static UUID GenerateRandomUUID()
	{
		thread_local static std::mt19937_64 Rng{std::random_device{}()};
		thread_local static std::uniform_int_distribution<uint64> Dist;
		return UUID{Dist(Rng), Dist(Rng)};
	}

	bool operator==(const UUID &Other) const
	{
		return this->Left == Other.Left && this->Right == Other.Right;
	}

	bool operator!=(const UUID &Other) const
	{
		return !(*this == Other);
	}

	[[nodiscard]] bool IsValid() const
	{
		return Left != 0 || Right != 0;
	}

	[[nodiscard]] explicit operator bool() const
	{
		return this->IsValid();
	}

	std::string ToString() const
	{
		return static_cast<std::string>(*this);
	}

	explicit operator std::string() const
	{
		char Buffer[37];
		snprintf(Buffer, sizeof(Buffer), "%08x-%04x-%04x-%04x-%012llx", (uint32)(Left >> 32), (uint16)(Left >> 16), (uint16)(Left & 0xFFFF),
				 (uint16)(Right >> 48), (unsigned long long)(Right & 0x0000FFFFFFFFFFFF));
		return std::string(Buffer);
	}

	friend std::hash<UUID>;
};
} // namespace util

namespace std
{
template <> struct hash<util::UUID>
{
	usize operator()(const util::UUID &ID) const
	{
		return std::hash<uint64>{}(ID.Left) ^ (std::hash<uint64>{}(ID.Right) << 1);
	}
};
} // namespace std
