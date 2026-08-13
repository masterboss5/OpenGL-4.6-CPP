#pragma once

#include "Source/core/EngineAPI.h"
#include "Source/types.h"

#include <span>
#include <stdexcept>
#include <vector>

namespace runtime::project
{
class ENGINE_API RuntimeSceneBinaryException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API RuntimeSceneBinary final
{
  public:
	static constexpr uint32 FormatVersion = 1;

	[[nodiscard]] static bool IsBinary(std::span<const uint8> Bytes) noexcept;
	[[nodiscard]] static std::vector<uint8> Compile(std::span<const uint8> JsonSource);
	[[nodiscard]] static string DecodeToJson(std::span<const uint8> BinaryScene);
};
} // namespace runtime::project
