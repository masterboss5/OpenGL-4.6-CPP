#pragma once

#include "Source/scene/Scene.h"
#include "Source/types.h"

namespace animation
{
class ENGINE_API AnimationSystem final
{
  public:
	void Update(world::Scene &Scene, float32 DeltaSeconds) const;
};
} // namespace animation
