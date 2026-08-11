#pragma once

namespace pipeline::device
{
class Device;
}

namespace pipeline::validation
{
// Opt-in deterministic render-core checks. Requires an initialized OpenGL
// context but creates no window-visible rendering work.
void RunDeterministicRenderCoreChecks(pipeline::device::Device &Device);
} // namespace pipeline::validation
