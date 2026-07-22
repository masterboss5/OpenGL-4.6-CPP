#include "GraphicsPipeline.h"

#include "ShaderException.h"
#include "src/pipeline/device/Device.h"
#include "src/pipeline/vertex/VertexDescriptor.h"

#include <algorithm>
#include <string>

namespace pipeline::shader
{
namespace
{
[[nodiscard]] GLenum ToGLCompare(CompareFunction Value)
{
	switch (Value)
	{
	case CompareFunction::Never:
		return GL_NEVER;
	case CompareFunction::Less:
		return GL_LESS;
	case CompareFunction::Equal:
		return GL_EQUAL;
	case CompareFunction::LessEqual:
		return GL_LEQUAL;
	case CompareFunction::Greater:
		return GL_GREATER;
	case CompareFunction::NotEqual:
		return GL_NOTEQUAL;
	case CompareFunction::GreaterEqual:
		return GL_GEQUAL;
	case CompareFunction::Always:
		return GL_ALWAYS;
	}
	return GL_ALWAYS;
}
[[nodiscard]] GLenum ToGLBlendFactor(BlendFactor Value)
{
	switch (Value)
	{
	case BlendFactor::Zero:
		return GL_ZERO;
	case BlendFactor::One:
		return GL_ONE;
	case BlendFactor::SourceColor:
		return GL_SRC_COLOR;
	case BlendFactor::OneMinusSourceColor:
		return GL_ONE_MINUS_SRC_COLOR;
	case BlendFactor::DestinationColor:
		return GL_DST_COLOR;
	case BlendFactor::OneMinusDestinationColor:
		return GL_ONE_MINUS_DST_COLOR;
	case BlendFactor::SourceAlpha:
		return GL_SRC_ALPHA;
	case BlendFactor::OneMinusSourceAlpha:
		return GL_ONE_MINUS_SRC_ALPHA;
	case BlendFactor::DestinationAlpha:
		return GL_DST_ALPHA;
	case BlendFactor::OneMinusDestinationAlpha:
		return GL_ONE_MINUS_DST_ALPHA;
	}
	return GL_ONE;
}
[[nodiscard]] GLenum ToGLBlendOperation(BlendOperation Value)
{
	switch (Value)
	{
	case BlendOperation::Add:
		return GL_FUNC_ADD;
	case BlendOperation::Subtract:
		return GL_FUNC_SUBTRACT;
	case BlendOperation::ReverseSubtract:
		return GL_FUNC_REVERSE_SUBTRACT;
	case BlendOperation::Minimum:
		return GL_MIN;
	case BlendOperation::Maximum:
		return GL_MAX;
	}
	return GL_FUNC_ADD;
}

[[nodiscard]] bool MatchesVertexInput(const renderer::VertexAttributeDescriptor &Attribute, GLenum ShaderType)
{
	const auto IsFloating = [&Attribute]()
	{
		return Attribute.Input == renderer::VertexAttributeInput::FloatingPoint &&
			   Attribute.DataType != renderer::VertexAttributeDataType::Float64;
	};
	const auto IsDouble = [&Attribute]()
	{
		return Attribute.Input == renderer::VertexAttributeInput::FloatingPoint &&
			   Attribute.DataType == renderer::VertexAttributeDataType::Float64;
	};
	const auto IsSignedInteger = [&Attribute]()
	{
		return Attribute.Input == renderer::VertexAttributeInput::Integer &&
			   (Attribute.DataType == renderer::VertexAttributeDataType::Int8 ||
				Attribute.DataType == renderer::VertexAttributeDataType::Int16 ||
				Attribute.DataType == renderer::VertexAttributeDataType::Int32);
	};
	const auto IsUnsignedInteger = [&Attribute]()
	{
		return Attribute.Input == renderer::VertexAttributeInput::Integer &&
			   (Attribute.DataType == renderer::VertexAttributeDataType::UInt8 ||
				Attribute.DataType == renderer::VertexAttributeDataType::UInt16 ||
				Attribute.DataType == renderer::VertexAttributeDataType::UInt32);
	};
	switch (ShaderType)
	{
	case GL_FLOAT:
		return IsFloating() && Attribute.ComponentCount == 1;
	case GL_FLOAT_VEC2:
		return IsFloating() && Attribute.ComponentCount == 2;
	case GL_FLOAT_VEC3:
		return IsFloating() && Attribute.ComponentCount == 3;
	case GL_FLOAT_VEC4:
		return IsFloating() && Attribute.ComponentCount == 4;
	case GL_DOUBLE:
		return IsDouble() && Attribute.ComponentCount == 1;
	case GL_DOUBLE_VEC2:
		return IsDouble() && Attribute.ComponentCount == 2;
	case GL_DOUBLE_VEC3:
		return IsDouble() && Attribute.ComponentCount == 3;
	case GL_DOUBLE_VEC4:
		return IsDouble() && Attribute.ComponentCount == 4;
	case GL_INT:
		return IsSignedInteger() && Attribute.ComponentCount == 1;
	case GL_INT_VEC2:
		return IsSignedInteger() && Attribute.ComponentCount == 2;
	case GL_INT_VEC3:
		return IsSignedInteger() && Attribute.ComponentCount == 3;
	case GL_INT_VEC4:
		return IsSignedInteger() && Attribute.ComponentCount == 4;
	case GL_UNSIGNED_INT:
		return IsUnsignedInteger() && Attribute.ComponentCount == 1;
	case GL_UNSIGNED_INT_VEC2:
		return IsUnsignedInteger() && Attribute.ComponentCount == 2;
	case GL_UNSIGNED_INT_VEC3:
		return IsUnsignedInteger() && Attribute.ComponentCount == 3;
	case GL_UNSIGNED_INT_VEC4:
		return IsUnsignedInteger() && Attribute.ComponentCount == 4;
	default:
		return false;
	}
}
} // namespace

GraphicsPipeline::GraphicsPipeline(device::Device &Device, const GraphicsPipelineDescriptor &Descriptor, const ShaderModule &Vertex,
								   const ShaderModule &Fragment)
	: Device(&Device), VertexProgramID(Vertex.GetProgramID()), Descriptor(Descriptor), VertexInputs(Vertex.GetVertexInputs()),
	  VertexUniformLocations(Vertex.GetUniformLocations())
{
	if (Vertex.GetStage() != ShaderStage::Vertex || Fragment.GetStage() != ShaderStage::Fragment)
		throw ShaderPipelineException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
									  "Graphics pipeline requires vertex and fragment modules");
	if (!Descriptor.State.ColorAttachmentBlends.empty() &&
		Descriptor.State.ColorAttachmentBlends.size() != Descriptor.State.RenderTargets.ColorAttachmentCount)
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "Per-attachment blend state must exactly match the render-target color attachment count");
	if (Descriptor.State.Multisample.SampleCount == 0 ||
		Descriptor.State.Multisample.SampleCount != Descriptor.State.RenderTargets.SampleCount)
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "Multisample state must specify the render-target sample count");
	if (Descriptor.State.Multisample.MinimumSampleShading < 0.0f || Descriptor.State.Multisample.MinimumSampleShading > 1.0f)
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "Minimum sample shading must be in the [0, 1] range");
	(void)this->Device->RequireCurrentContext();
	glCreateProgramPipelines(1, &this->PipelineID);
	glUseProgramStages(this->PipelineID, GL_VERTEX_SHADER_BIT, Vertex.GetProgramID());
	glUseProgramStages(this->PipelineID, GL_FRAGMENT_SHADER_BIT, Fragment.GetProgramID());
	glValidateProgramPipeline(this->PipelineID);
	GLint Valid = GL_FALSE;
	glGetProgramPipelineiv(this->PipelineID, GL_VALIDATE_STATUS, &Valid);
	if (Valid != GL_TRUE)
	{
		glDeleteProgramPipelines(1, &this->PipelineID);
		this->PipelineID = 0;
		throw ShaderPipelineException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
									  "OpenGL rejected the separable program pipeline");
	}
	this->Device->CheckErrors("GraphicsPipeline creation");
}
void GraphicsPipeline::SetVertexUniformUInt(string_view Name, uint32 Value) const
{
	(void)this->Device->RequireCurrentContext();
	const auto LocationIt = this->VertexUniformLocations.find(std::string(Name));
	if (LocationIt != this->VertexUniformLocations.end())
	{
		glProgramUniform1ui(this->VertexProgramID, LocationIt->second, Value);
	}
}

void GraphicsPipeline::ValidateVertexDescriptor(const renderer::VertexDescriptor &VertexDescriptor) const
{
	const uint64 LayoutHash = VertexDescriptor.GetLayoutHash();
	if (ValidatedVertexLayouts.contains(LayoutHash))
		return;
	for (const ShaderModule::VertexInput &Input : this->VertexInputs)
	{
		const auto Attributes = VertexDescriptor.GetAttributes();
		const auto Attribute =
			std::find_if(Attributes.begin(), Attributes.end(), [&Input](const renderer::VertexAttributeDescriptor &Candidate)
						 { return Candidate.Location == static_cast<GLuint>(Input.Location); });
		if (Attribute == Attributes.end() || !MatchesVertexInput(*Attribute, Input.Type))
		{
			throw ShaderInterfaceException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
										   "VertexDescriptor is incompatible with required vertex input location " +
											   std::to_string(Input.Location));
		}
	}
	ValidatedVertexLayouts.emplace(LayoutHash);
}
GraphicsPipeline::~GraphicsPipeline()
{
	if (this->PipelineID != 0)
	{
		if (this->Device != nullptr && this->Device->CanIssueCommands())
			glDeleteProgramPipelines(1, &this->PipelineID);
		this->PipelineID = 0;
	}
}
void GraphicsPipeline::Bind() const
{
	(void)this->Device->RequireCurrentContext();
	// A program bound by a prior compute pass overrides a program pipeline.
	// Clear it before binding separable graphics stages.
	glUseProgram(0);
	glBindProgramPipeline(this->PipelineID);
	// Color-write state is global OpenGL state. Every graphics pipeline must
	// establish it explicitly instead of inheriting a depth-only or external
	// pass's mask.
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (Descriptor.State.DepthStencil.DepthTest)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
	glDepthMask(Descriptor.State.DepthStencil.DepthWrite ? GL_TRUE : GL_FALSE);
	glDepthFunc(ToGLCompare(Descriptor.State.DepthStencil.DepthCompare));
	if (Descriptor.State.ColorAttachmentBlends.empty())
	{
		if (Descriptor.State.Blend.Enabled)
		{
			glEnable(GL_BLEND);
			glBlendFuncSeparate(
				ToGLBlendFactor(Descriptor.State.Blend.SourceColor), ToGLBlendFactor(Descriptor.State.Blend.DestinationColor),
				ToGLBlendFactor(Descriptor.State.Blend.SourceAlpha), ToGLBlendFactor(Descriptor.State.Blend.DestinationAlpha));
			glBlendEquationSeparate(ToGLBlendOperation(Descriptor.State.Blend.ColorOperation),
									ToGLBlendOperation(Descriptor.State.Blend.AlphaOperation));
		}
		else
			glDisable(GL_BLEND);
	}
	else
	{
		glDisable(GL_BLEND);
		for (uint32 AttachmentIndex = 0; AttachmentIndex < Descriptor.State.RenderTargets.ColorAttachmentCount; ++AttachmentIndex)
		{
			const BlendState &Blend = Descriptor.State.ColorAttachmentBlends[AttachmentIndex];
			if (Blend.Enabled)
				glEnablei(GL_BLEND, AttachmentIndex);
			else
				glDisablei(GL_BLEND, AttachmentIndex);
			glBlendFuncSeparatei(AttachmentIndex, ToGLBlendFactor(Blend.SourceColor), ToGLBlendFactor(Blend.DestinationColor),
								 ToGLBlendFactor(Blend.SourceAlpha), ToGLBlendFactor(Blend.DestinationAlpha));
			glBlendEquationSeparatei(AttachmentIndex, ToGLBlendOperation(Blend.ColorOperation), ToGLBlendOperation(Blend.AlphaOperation));
		}
	}
	glPolygonMode(GL_FRONT_AND_BACK, Descriptor.State.Rasterizer.Wireframe ? GL_LINE : GL_FILL);
	if (Descriptor.State.Rasterizer.CullMode == CullMode::None)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(Descriptor.State.Rasterizer.CullMode == CullMode::Back ? GL_BACK : GL_FRONT);
	}
	glFrontFace(Descriptor.State.Rasterizer.FrontFace == FrontFace::CounterClockwise ? GL_CCW : GL_CW);
	if (Descriptor.State.Multisample.SampleCount > 1)
		glEnable(GL_MULTISAMPLE);
	else
		glDisable(GL_MULTISAMPLE);
	if (Descriptor.State.Multisample.SampleShading)
	{
		glEnable(GL_SAMPLE_SHADING);
		glMinSampleShading(Descriptor.State.Multisample.MinimumSampleShading);
	}
	else
		glDisable(GL_SAMPLE_SHADING);
	if (Descriptor.State.Multisample.AlphaToCoverage)
		glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	else
		glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	if (Descriptor.State.Multisample.AlphaToOne)
		glEnable(GL_SAMPLE_ALPHA_TO_ONE);
	else
		glDisable(GL_SAMPLE_ALPHA_TO_ONE);
}
GLenum GraphicsPipeline::GetGLTopology() const noexcept
{
	switch (Descriptor.State.Topology)
	{
	case PrimitiveTopology::TriangleList:
		return GL_TRIANGLES;
	case PrimitiveTopology::TriangleStrip:
		return GL_TRIANGLE_STRIP;
	case PrimitiveTopology::LineList:
		return GL_LINES;
	case PrimitiveTopology::PointList:
		return GL_POINTS;
	}
	return GL_TRIANGLES;
}
const GraphicsPipelineDescriptor &GraphicsPipeline::GetDescriptor() const noexcept
{
	return this->Descriptor;
}
} // namespace pipeline::shader
