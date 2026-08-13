#pragma once

#include "Source/core/EngineAPI.h"

#include "RenderGraph.h"

namespace pipeline::graph
{
// A render pass owns only its graph resource declarations and execution
// callback. GPU pipelines remain cached objects selected by the callback.
class ENGINE_API RenderPass final
{
  public:
	explicit RenderPass(RenderPassDescription Description);
	~RenderPass() = default;

	RenderPass(const RenderPass &) = delete;
	RenderPass &operator=(const RenderPass &) = delete;
	RenderPass(RenderPass &&) noexcept = default;
	RenderPass &operator=(RenderPass &&) noexcept = default;

	[[nodiscard]] const RenderPassDescription &GetDescription() const noexcept;

  private:
	friend class RenderGraph;
	RenderPass() = default;
	void ResetForReuse() noexcept;
	[[nodiscard]] RenderPassDescription &GetMutableDescription() noexcept;

	RenderPassDescription Description;
};
} // namespace pipeline::graph
