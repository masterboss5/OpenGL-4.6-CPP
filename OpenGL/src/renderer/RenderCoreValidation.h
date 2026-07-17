#pragma once

namespace renderer::validation
{
	// Opt-in deterministic render-core checks. Requires an initialized OpenGL
	// context but creates no window-visible rendering work.
	void runDeterministicRenderCoreChecks();
}
