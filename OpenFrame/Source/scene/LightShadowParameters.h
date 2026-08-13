#pragma once

#include "Source/core/EngineAPI.h"
#include "Source/types.h"

#include <type_traits>

struct alignas(16) ENGINE_API LightShadowParameters final
{
	uint32 Resolution = 2'048;
	float32 ConstantBias = 0.0005f;
	float32 SlopeBias = 1.5f;
	float32 NormalBias = 0.02f;
	float32 FilterRadius = 1.0f;
	uint32 Padding0 = 0;
	uint32 Padding1 = 0;
	uint32 Padding2 = 0;
};

static_assert(std::is_trivially_copyable_v<LightShadowParameters>, "Light shadow parameters must remain trivially copyable");
static_assert(sizeof(LightShadowParameters) == 32U, "Light shadow parameters must preserve a packed 16-byte-aligned layout");
