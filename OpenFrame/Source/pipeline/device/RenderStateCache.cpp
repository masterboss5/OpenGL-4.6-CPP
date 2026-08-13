#include "RenderStateCache.h"

#include <algorithm>

namespace pipeline::device
{
namespace
{
[[nodiscard]] GLenum ToGLCompare(const pipeline::shader::CompareFunction Value) noexcept
{
	switch (Value)
	{
	case pipeline::shader::CompareFunction::Never:
		return GL_NEVER;
	case pipeline::shader::CompareFunction::Less:
		return GL_LESS;
	case pipeline::shader::CompareFunction::Equal:
		return GL_EQUAL;
	case pipeline::shader::CompareFunction::LessEqual:
		return GL_LEQUAL;
	case pipeline::shader::CompareFunction::Greater:
		return GL_GREATER;
	case pipeline::shader::CompareFunction::NotEqual:
		return GL_NOTEQUAL;
	case pipeline::shader::CompareFunction::GreaterEqual:
		return GL_GEQUAL;
	case pipeline::shader::CompareFunction::Always:
		return GL_ALWAYS;
	}
	return GL_ALWAYS;
}

[[nodiscard]] GLenum ToGLBlendFactor(const pipeline::shader::BlendFactor Value) noexcept
{
	switch (Value)
	{
	case pipeline::shader::BlendFactor::Zero:
		return GL_ZERO;
	case pipeline::shader::BlendFactor::One:
		return GL_ONE;
	case pipeline::shader::BlendFactor::SourceColor:
		return GL_SRC_COLOR;
	case pipeline::shader::BlendFactor::OneMinusSourceColor:
		return GL_ONE_MINUS_SRC_COLOR;
	case pipeline::shader::BlendFactor::DestinationColor:
		return GL_DST_COLOR;
	case pipeline::shader::BlendFactor::OneMinusDestinationColor:
		return GL_ONE_MINUS_DST_COLOR;
	case pipeline::shader::BlendFactor::SourceAlpha:
		return GL_SRC_ALPHA;
	case pipeline::shader::BlendFactor::OneMinusSourceAlpha:
		return GL_ONE_MINUS_SRC_ALPHA;
	case pipeline::shader::BlendFactor::DestinationAlpha:
		return GL_DST_ALPHA;
	case pipeline::shader::BlendFactor::OneMinusDestinationAlpha:
		return GL_ONE_MINUS_DST_ALPHA;
	}
	return GL_ONE;
}

[[nodiscard]] GLenum ToGLBlendOperation(const pipeline::shader::BlendOperation Value) noexcept
{
	switch (Value)
	{
	case pipeline::shader::BlendOperation::Add:
		return GL_FUNC_ADD;
	case pipeline::shader::BlendOperation::Subtract:
		return GL_FUNC_SUBTRACT;
	case pipeline::shader::BlendOperation::ReverseSubtract:
		return GL_FUNC_REVERSE_SUBTRACT;
	case pipeline::shader::BlendOperation::Minimum:
		return GL_MIN;
	case pipeline::shader::BlendOperation::Maximum:
		return GL_MAX;
	}
	return GL_FUNC_ADD;
}

[[nodiscard]] GLenum ToGLStencilOperation(const pipeline::shader::StencilOperation Value) noexcept
{
	switch (Value)
	{
	case pipeline::shader::StencilOperation::Keep:
		return GL_KEEP;
	case pipeline::shader::StencilOperation::Zero:
		return GL_ZERO;
	case pipeline::shader::StencilOperation::Replace:
		return GL_REPLACE;
	case pipeline::shader::StencilOperation::IncrementClamp:
		return GL_INCR;
	case pipeline::shader::StencilOperation::DecrementClamp:
		return GL_DECR;
	case pipeline::shader::StencilOperation::Invert:
		return GL_INVERT;
	case pipeline::shader::StencilOperation::IncrementWrap:
		return GL_INCR_WRAP;
	case pipeline::shader::StencilOperation::DecrementWrap:
		return GL_DECR_WRAP;
	}
	return GL_KEEP;
}

[[nodiscard]] bool Equal(const pipeline::shader::RasterizerState &Left, const pipeline::shader::RasterizerState &Right) noexcept
{
	return Left.CullMode == Right.CullMode && Left.FrontFace == Right.FrontFace && Left.Wireframe == Right.Wireframe;
}

[[nodiscard]] bool Equal(const pipeline::shader::DepthStencilState &Left, const pipeline::shader::DepthStencilState &Right) noexcept
{
	const auto EqualFace = [](const pipeline::shader::StencilFaceState &First, const pipeline::shader::StencilFaceState &Second)
	{
		return First.Compare == Second.Compare && First.Fail == Second.Fail && First.DepthFail == Second.DepthFail &&
			   First.Pass == Second.Pass && First.Reference == Second.Reference && First.ReadMask == Second.ReadMask &&
			   First.WriteMask == Second.WriteMask;
	};
	return Left.DepthTest == Right.DepthTest && Left.DepthWrite == Right.DepthWrite && Left.DepthCompare == Right.DepthCompare &&
		   Left.StencilTest == Right.StencilTest && EqualFace(Left.FrontStencil, Right.FrontStencil) &&
		   EqualFace(Left.BackStencil, Right.BackStencil);
}

[[nodiscard]] bool Equal(const pipeline::shader::BlendState &Left, const pipeline::shader::BlendState &Right) noexcept
{
	return Left.Enabled == Right.Enabled && Left.SourceColor == Right.SourceColor && Left.DestinationColor == Right.DestinationColor &&
		   Left.SourceAlpha == Right.SourceAlpha && Left.DestinationAlpha == Right.DestinationAlpha &&
		   Left.ColorOperation == Right.ColorOperation && Left.AlphaOperation == Right.AlphaOperation;
}

[[nodiscard]] bool Equal(const pipeline::shader::MultisampleState &Left, const pipeline::shader::MultisampleState &Right) noexcept
{
	return Left.SampleCount == Right.SampleCount && Left.SampleShading == Right.SampleShading &&
		   Left.MinimumSampleShading == Right.MinimumSampleShading && Left.AlphaToCoverage == Right.AlphaToCoverage &&
		   Left.AlphaToOne == Right.AlphaToOne;
}

[[nodiscard]] bool Equal(const std::vector<pipeline::shader::BlendState> &Left,
						 const std::vector<pipeline::shader::BlendState> &Right) noexcept
{
	if (Left.size() != Right.size())
		return false;
	for (usize Index = 0; Index < Left.size(); ++Index)
		if (!Equal(Left[Index], Right[Index]))
			return false;
	return true;
}
} // namespace

RenderStateCache::RenderStateCache(const usize MaximumBlendAttachments)
{
	// Reserve the device-reported MRT range so ordinary pipeline binds do not
	// allocate while tracking per-attachment blend state.
	this->LastColorAttachmentBlends.reserve(std::max<usize>(8, MaximumBlendAttachments));
}

void RenderStateCache::Apply(const pipeline::shader::GraphicsPipelineState &State)
{
	const bool RasterizerChanged = !this->Valid || !Equal(this->LastRasterizer, State.Rasterizer);
	const bool DepthStencilChanged = !this->Valid || !Equal(this->LastDepthStencil, State.DepthStencil);
	const bool BlendChanged =
		!this->Valid || !Equal(this->LastBlend, State.Blend) || !Equal(this->LastColorAttachmentBlends, State.ColorAttachmentBlends);
	const bool MultisampleChanged = !this->Valid || !Equal(this->LastMultisample, State.Multisample);
	if (!this->Valid)
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (RasterizerChanged)
		ApplyRasterizer(State.Rasterizer);
	if (DepthStencilChanged)
		ApplyDepthStencil(State.DepthStencil);
	if (BlendChanged)
		ApplyBlend(State, this->LastColorAttachmentBlends);
	if (MultisampleChanged)
		ApplyMultisample(State.Multisample);

	this->LastRasterizer = State.Rasterizer;
	this->LastDepthStencil = State.DepthStencil;
	this->LastBlend = State.Blend;
	if (BlendChanged)
		this->LastColorAttachmentBlends.assign(State.ColorAttachmentBlends.begin(), State.ColorAttachmentBlends.end());
	this->LastMultisample = State.Multisample;
	this->Valid = true;
}

void RenderStateCache::Invalidate() noexcept
{
	this->Valid = false;
}

void RenderStateCache::ApplyRasterizer(const pipeline::shader::RasterizerState &State)
{
	glPolygonMode(GL_FRONT_AND_BACK, State.Wireframe ? GL_LINE : GL_FILL);
	if (State.CullMode == pipeline::shader::CullMode::None)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(State.CullMode == pipeline::shader::CullMode::Back ? GL_BACK : GL_FRONT);
	}
	glFrontFace(State.FrontFace == pipeline::shader::FrontFace::CounterClockwise ? GL_CCW : GL_CW);
}

void RenderStateCache::ApplyDepthStencil(const pipeline::shader::DepthStencilState &State)
{
	if (State.DepthTest)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
	glDepthMask(State.DepthWrite ? GL_TRUE : GL_FALSE);
	glDepthFunc(ToGLCompare(State.DepthCompare));
	if (State.StencilTest)
		glEnable(GL_STENCIL_TEST);
	else
		glDisable(GL_STENCIL_TEST);
	const auto ApplyStencilFace = [](const GLenum Face, const pipeline::shader::StencilFaceState &Stencil)
	{
		glStencilFuncSeparate(Face, ToGLCompare(Stencil.Compare), static_cast<GLint>(Stencil.Reference), Stencil.ReadMask);
		glStencilOpSeparate(Face, ToGLStencilOperation(Stencil.Fail), ToGLStencilOperation(Stencil.DepthFail),
							ToGLStencilOperation(Stencil.Pass));
		glStencilMaskSeparate(Face, Stencil.WriteMask);
	};
	ApplyStencilFace(GL_FRONT, State.FrontStencil);
	ApplyStencilFace(GL_BACK, State.BackStencil);
}

void RenderStateCache::ApplyBlend(const pipeline::shader::GraphicsPipelineState &State,
								  const std::vector<pipeline::shader::BlendState> &Previous)
{
	const usize AttachmentCount = std::max(Previous.size(), State.ColorAttachmentBlends.size());
	if (State.ColorAttachmentBlends.empty())
	{
		for (usize Index = 0; Index < AttachmentCount; ++Index)
			glDisablei(GL_BLEND, static_cast<GLuint>(Index));
		if (State.Blend.Enabled)
		{
			glEnable(GL_BLEND);
			glBlendFuncSeparate(ToGLBlendFactor(State.Blend.SourceColor), ToGLBlendFactor(State.Blend.DestinationColor),
								ToGLBlendFactor(State.Blend.SourceAlpha), ToGLBlendFactor(State.Blend.DestinationAlpha));
			glBlendEquationSeparate(ToGLBlendOperation(State.Blend.ColorOperation), ToGLBlendOperation(State.Blend.AlphaOperation));
		}
		else
			glDisable(GL_BLEND);
		return;
	}

	glDisable(GL_BLEND);
	for (usize Index = 0; Index < AttachmentCount; ++Index)
	{
		if (Index >= State.ColorAttachmentBlends.size())
		{
			glDisablei(GL_BLEND, static_cast<GLuint>(Index));
			continue;
		}
		const pipeline::shader::BlendState &Blend = State.ColorAttachmentBlends[Index];
		if (Blend.Enabled)
			glEnablei(GL_BLEND, static_cast<GLuint>(Index));
		else
			glDisablei(GL_BLEND, static_cast<GLuint>(Index));
		glBlendFuncSeparatei(static_cast<GLuint>(Index), ToGLBlendFactor(Blend.SourceColor), ToGLBlendFactor(Blend.DestinationColor),
							 ToGLBlendFactor(Blend.SourceAlpha), ToGLBlendFactor(Blend.DestinationAlpha));
		glBlendEquationSeparatei(static_cast<GLuint>(Index), ToGLBlendOperation(Blend.ColorOperation),
								 ToGLBlendOperation(Blend.AlphaOperation));
	}
}

void RenderStateCache::ApplyMultisample(const pipeline::shader::MultisampleState &State)
{
	if (State.SampleCount > 1)
		glEnable(GL_MULTISAMPLE);
	else
		glDisable(GL_MULTISAMPLE);
	if (State.SampleShading)
	{
		glEnable(GL_SAMPLE_SHADING);
		glMinSampleShading(State.MinimumSampleShading);
	}
	else
		glDisable(GL_SAMPLE_SHADING);
	if (State.AlphaToCoverage)
		glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	else
		glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	if (State.AlphaToOne)
		glEnable(GL_SAMPLE_ALPHA_TO_ONE);
	else
		glDisable(GL_SAMPLE_ALPHA_TO_ONE);
}
} // namespace pipeline::device
