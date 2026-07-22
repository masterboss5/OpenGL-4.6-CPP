#pragma once

namespace pipeline::device
{
class Device;
}

namespace renderer::validation
{
// Opt-in deterministic render-core checks. Requires an initialized OpenGL
// context but creates no window-visible rendering work.
void RunDeterministicRenderCoreChecks(pipeline::device::Device &Device);
} // namespace renderer::validation
