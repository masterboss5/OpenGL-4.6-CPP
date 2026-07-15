#pragma once
#include "src/types.h"
#include <random>
#include <functional>
#include <string>

namespace util
{
	class UUID final
	{
	private:
		uint64 left = 0;
		uint64 right = 0;
	public:

		UUID() = default;
		UUID(uint64 left, uint64 right)
			: left(left), right(right)
		{
		}

		inline static UUID generateRandomUUID()
		{
			thread_local static std::mt19937_64 rng{ std::random_device{}() };
			thread_local static std::uniform_int_distribution<uint64> dist;
			return UUID {dist(rng), dist(rng)};
		}

		bool operator==(const UUID& other) const
		{
			return this->left == other.left && this->right == other.right;
		}

		bool operator!=(const UUID& other) const
		{
			return !(*this == other);
		}


		bool isValid() const
		{
			return left != 0 || right != 0;
		}

		explicit operator bool() const
		{
			return this->isValid();
		}

		std::string toString() const
		{
			return static_cast<std::string>(*this);
		}

		explicit operator std::string() const
		{
			char buffer[37];
			snprintf(buffer, sizeof(buffer),
				"%08x-%04x-%04x-%04x-%012llx",
				(uint32)(left >> 32),
				(uint16)(left >> 16),
				(uint16)(left & 0xFFFF),
				(uint16)(right >> 48),
				(unsigned long long)(right & 0x0000FFFFFFFFFFFF));
			return std::string(buffer);
		}

		friend std::hash<UUID>;
	};
}

namespace std
{
	template<>
	struct hash<util::UUID>
	{
		size_t operator()(const util::UUID& id) const
		{
			return std::hash<uint64>{}(id.left) ^ (std::hash<uint64>{}(id.right) << 1);
		}
	};
}