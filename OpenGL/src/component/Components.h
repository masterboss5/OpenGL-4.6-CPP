#pragma once
#include "src/Types.h"

namespace components
{
	enum class CObjectComponentTypeID : uint32
	{
		CObjectTransformComponent = 0,
		CObjectMeshComponent = 1
	};

	inline constexpr uint32 MAX_COBJECT_COMPONENTS = 2;
}