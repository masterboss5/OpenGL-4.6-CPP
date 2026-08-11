#pragma once

#include "src/core/EngineAPI.h"
#include "src/pipeline/shader/GraphicsPipeline.h"

#include <vector>

namespace pipeline::device
{
class ENGINE_API RenderStateCache final
{
  public:
	explicit RenderStateCache(usize MaximumBlendAttachments = 8);
	RenderStateCache(const RenderStateCache &) = delete;
	RenderStateCache &operator=(const RenderStateCache &) = delete;

	void Apply(const pipeline::shader::GraphicsPipelineState &State);
	void Invalidate() noexcept;

  private:
	static void ApplyRasterizer(const pipeline::shader::RasterizerState &State);
	static void ApplyDepthStencil(const pipeline::shader::DepthStencilState &State);
	static void ApplyBlend(const pipeline::shader::GraphicsPipelineState &State, const std::vector<pipeline::shader::BlendState> &Previous);
	static void ApplyMultisample(const pipeline::shader::MultisampleState &State);

	bool Valid = false;
	pipeline::shader::RasterizerState LastRasterizer;
	pipeline::shader::DepthStencilState LastDepthStencil;
	pipeline::shader::BlendState LastBlend;
	std::vector<pipeline::shader::BlendState> LastColorAttachmentBlends;
	pipeline::shader::MultisampleState LastMultisample;
};
} // namespace pipeline::device
