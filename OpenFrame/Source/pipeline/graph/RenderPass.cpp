#include "RenderPass.h"

#include <stdexcept>
#include <utility>

namespace pipeline::graph
{
RenderPass::RenderPass(RenderPassDescription Description) : Description(std::move(Description))
{
	if (this->Description.Name.empty() || !this->Description.Execute)
		throw std::invalid_argument("Render pass requires a name and execute callback");
}

const RenderPassDescription &RenderPass::GetDescription() const noexcept
{
	return this->Description;
}

void RenderPass::ResetForReuse() noexcept
{
	this->Description.Name.clear();
	this->Description.ReadTextures.clear();
	this->Description.ReadBuffers.clear();
	this->Description.ColorAttachments.clear();
	this->Description.DepthAttachment.reset();
	this->Description.WriteTextures.clear();
	this->Description.WriteBuffers.clear();
	this->Description.Execute = {};
	this->Description.Queue = PassQueue::Graphics;
}

RenderPassDescription &RenderPass::GetMutableDescription() noexcept
{
	return this->Description;
}
} // namespace pipeline::graph
