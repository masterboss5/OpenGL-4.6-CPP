#pragma once

#include "src/types.h"

namespace core
{
inline constexpr uint32 EngineABIVersion = 1;

[[nodiscard]] ENGINE_API uint32 GetEngineABIVersion() noexcept;
} // namespace core
