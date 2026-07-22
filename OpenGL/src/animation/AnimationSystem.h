#pragma once

#include "src/scene/Scene.h"
#include "src/types.h"

namespace animation
{
class AnimationSystem final
{
  public:
	void Update(world::Scene &Scene, float32 DeltaSeconds) const;
};
} // namespace animation
