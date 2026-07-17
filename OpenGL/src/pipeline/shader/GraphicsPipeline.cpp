#include "GraphicsPipeline.h"

#include <algorithm>
#include <string>

#include "ShaderException.h"
#include "src/pipeline/device/OpenGLRuntime.h"
#include "src/pipeline/vertex/VertexDescriptor.h"

namespace pipeline::shader
{
	namespace
	{
		[[nodiscard]] GLenum toGLCompare(CompareFunction value) { switch (value) { case CompareFunction::Never: return GL_NEVER; case CompareFunction::Less: return GL_LESS; case CompareFunction::Equal: return GL_EQUAL; case CompareFunction::LessEqual: return GL_LEQUAL; case CompareFunction::Greater: return GL_GREATER; case CompareFunction::NotEqual: return GL_NOTEQUAL; case CompareFunction::GreaterEqual: return GL_GEQUAL; case CompareFunction::Always: return GL_ALWAYS; } return GL_ALWAYS; }
		[[nodiscard]] GLenum toGLBlendFactor(BlendFactor value) { switch (value) { case BlendFactor::Zero: return GL_ZERO; case BlendFactor::One: return GL_ONE; case BlendFactor::SourceColor: return GL_SRC_COLOR; case BlendFactor::OneMinusSourceColor: return GL_ONE_MINUS_SRC_COLOR; case BlendFactor::DestinationColor: return GL_DST_COLOR; case BlendFactor::OneMinusDestinationColor: return GL_ONE_MINUS_DST_COLOR; case BlendFactor::SourceAlpha: return GL_SRC_ALPHA; case BlendFactor::OneMinusSourceAlpha: return GL_ONE_MINUS_SRC_ALPHA; case BlendFactor::DestinationAlpha: return GL_DST_ALPHA; case BlendFactor::OneMinusDestinationAlpha: return GL_ONE_MINUS_DST_ALPHA; } return GL_ONE; }
		[[nodiscard]] GLenum toGLBlendOperation(BlendOperation value) { switch (value) { case BlendOperation::Add: return GL_FUNC_ADD; case BlendOperation::Subtract: return GL_FUNC_SUBTRACT; case BlendOperation::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT; case BlendOperation::Minimum: return GL_MIN; case BlendOperation::Maximum: return GL_MAX; } return GL_FUNC_ADD; }

		[[nodiscard]] bool matchesVertexInput(const renderer::VertexAttributeDescriptor& attribute, GLenum shaderType)
		{
			const auto isFloating = [&attribute]() { return attribute.input == renderer::VertexAttributeInput::FloatingPoint && attribute.dataType != renderer::VertexAttributeDataType::Float64; };
			const auto isDouble = [&attribute]() { return attribute.input == renderer::VertexAttributeInput::FloatingPoint && attribute.dataType == renderer::VertexAttributeDataType::Float64; };
			const auto isSignedInteger = [&attribute]() { return attribute.input == renderer::VertexAttributeInput::Integer && (attribute.dataType == renderer::VertexAttributeDataType::Int8 || attribute.dataType == renderer::VertexAttributeDataType::Int16 || attribute.dataType == renderer::VertexAttributeDataType::Int32); };
			const auto isUnsignedInteger = [&attribute]() { return attribute.input == renderer::VertexAttributeInput::Integer && (attribute.dataType == renderer::VertexAttributeDataType::UInt8 || attribute.dataType == renderer::VertexAttributeDataType::UInt16 || attribute.dataType == renderer::VertexAttributeDataType::UInt32); };
			switch (shaderType)
			{
			case GL_FLOAT: return isFloating() && attribute.componentCount == 1;
			case GL_FLOAT_VEC2: return isFloating() && attribute.componentCount == 2;
			case GL_FLOAT_VEC3: return isFloating() && attribute.componentCount == 3;
			case GL_FLOAT_VEC4: return isFloating() && attribute.componentCount == 4;
			case GL_DOUBLE: return isDouble() && attribute.componentCount == 1;
			case GL_DOUBLE_VEC2: return isDouble() && attribute.componentCount == 2;
			case GL_DOUBLE_VEC3: return isDouble() && attribute.componentCount == 3;
			case GL_DOUBLE_VEC4: return isDouble() && attribute.componentCount == 4;
			case GL_INT: return isSignedInteger() && attribute.componentCount == 1;
			case GL_INT_VEC2: return isSignedInteger() && attribute.componentCount == 2;
			case GL_INT_VEC3: return isSignedInteger() && attribute.componentCount == 3;
			case GL_INT_VEC4: return isSignedInteger() && attribute.componentCount == 4;
			case GL_UNSIGNED_INT: return isUnsignedInteger() && attribute.componentCount == 1;
			case GL_UNSIGNED_INT_VEC2: return isUnsignedInteger() && attribute.componentCount == 2;
			case GL_UNSIGNED_INT_VEC3: return isUnsignedInteger() && attribute.componentCount == 3;
			case GL_UNSIGNED_INT_VEC4: return isUnsignedInteger() && attribute.componentCount == 4;
			default: return false;
			}
		}
	}

	GraphicsPipeline::GraphicsPipeline(const GraphicsPipelineDescriptor& descriptor, const ShaderModule& vertex, const ShaderModule& fragment) : vertexProgramID(vertex.getProgramID()), descriptor(descriptor), vertexInputs(vertex.getVertexInputs())
	{
		if (vertex.getStage() != ShaderStage::Vertex || fragment.getStage() != ShaderStage::Fragment) throw ShaderPipelineException(ShaderStage::Vertex, descriptor.vertex.path, descriptor.permutation, "Graphics pipeline requires vertex and fragment modules");
		if (!descriptor.state.colorAttachmentBlends.empty() && descriptor.state.colorAttachmentBlends.size() != descriptor.state.renderTargets.colorAttachmentCount) throw ShaderPipelineException(ShaderStage::Fragment, descriptor.fragment.path, descriptor.permutation, "Per-attachment blend state must exactly match the render-target color attachment count");
		if (descriptor.state.multisample.sampleCount == 0 || descriptor.state.multisample.sampleCount != descriptor.state.renderTargets.sampleCount) throw ShaderPipelineException(ShaderStage::Fragment, descriptor.fragment.path, descriptor.permutation, "Multisample state must specify the render-target sample count");
		if (descriptor.state.multisample.minimumSampleShading < 0.0f || descriptor.state.multisample.minimumSampleShading > 1.0f) throw ShaderPipelineException(ShaderStage::Fragment, descriptor.fragment.path, descriptor.permutation, "Minimum sample shading must be in the [0, 1] range");
		device::requireOpenGL46Context(); glCreateProgramPipelines(1, &this->pipelineID);
		glUseProgramStages(this->pipelineID, GL_VERTEX_SHADER_BIT, vertex.getProgramID()); glUseProgramStages(this->pipelineID, GL_FRAGMENT_SHADER_BIT, fragment.getProgramID());
		glValidateProgramPipeline(this->pipelineID); GLint valid = GL_FALSE; glGetProgramPipelineiv(this->pipelineID, GL_VALIDATE_STATUS, &valid);
		if (valid != GL_TRUE) { glDeleteProgramPipelines(1, &this->pipelineID); this->pipelineID = 0; throw ShaderPipelineException(ShaderStage::Vertex, descriptor.vertex.path, descriptor.permutation, "OpenGL rejected the separable program pipeline"); }
	}
	void GraphicsPipeline::setVertexUniformUInt(string_view name, uint32 value) const
	{
		const std::string uniformName(name);
		const auto [locationIt, inserted] = vertexUniformLocations.try_emplace(uniformName, -1);
		if (inserted)
		{
			locationIt->second = glGetUniformLocation(this->vertexProgramID, uniformName.c_str());
		}
		if (locationIt->second >= 0)
		{
			glProgramUniform1ui(this->vertexProgramID, locationIt->second, value);
		}
	}

	void GraphicsPipeline::validateVertexDescriptor(const renderer::VertexDescriptor& vertexDescriptor) const
	{
		const uint64 layoutHash = vertexDescriptor.getLayoutHash();
		if (validatedVertexLayouts.contains(layoutHash)) return;
		for (const ShaderModule::VertexInput& input : this->vertexInputs)
		{
			const auto attributes = vertexDescriptor.getAttributes();
			const auto attribute = std::find_if(attributes.begin(), attributes.end(), [&input](const renderer::VertexAttributeDescriptor& candidate) { return candidate.location == static_cast<GLuint>(input.location); });
			if (attribute == attributes.end() || !matchesVertexInput(*attribute, input.type))
			{
				throw ShaderInterfaceException(ShaderStage::Vertex, descriptor.vertex.path, descriptor.permutation, "VertexDescriptor is incompatible with required vertex input location " + std::to_string(input.location));
			}
		}
		validatedVertexLayouts.emplace(layoutHash);
	}
	GraphicsPipeline::~GraphicsPipeline() { if (this->pipelineID != 0) glDeleteProgramPipelines(1, &this->pipelineID); }
	void GraphicsPipeline::bind() const
	{
		// A program bound by a prior compute pass overrides a program pipeline.
		// Clear it before binding separable graphics stages.
		glUseProgram(0);
		glBindProgramPipeline(this->pipelineID);
		// Color-write state is global OpenGL state. Every graphics pipeline must
		// establish it explicitly instead of inheriting a depth-only or external
		// pass's mask.
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		if (descriptor.state.depthStencil.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
		glDepthMask(descriptor.state.depthStencil.depthWrite ? GL_TRUE : GL_FALSE); glDepthFunc(toGLCompare(descriptor.state.depthStencil.depthCompare));
		if (descriptor.state.colorAttachmentBlends.empty())
		{
			if (descriptor.state.blend.enabled) { glEnable(GL_BLEND); glBlendFuncSeparate(toGLBlendFactor(descriptor.state.blend.sourceColor), toGLBlendFactor(descriptor.state.blend.destinationColor), toGLBlendFactor(descriptor.state.blend.sourceAlpha), toGLBlendFactor(descriptor.state.blend.destinationAlpha)); glBlendEquationSeparate(toGLBlendOperation(descriptor.state.blend.colorOperation), toGLBlendOperation(descriptor.state.blend.alphaOperation)); } else glDisable(GL_BLEND);
		}
		else
		{
			glDisable(GL_BLEND);
			for (uint32 attachmentIndex = 0; attachmentIndex < descriptor.state.renderTargets.colorAttachmentCount; ++attachmentIndex)
			{
				const BlendState& blend = descriptor.state.colorAttachmentBlends[attachmentIndex];
				if (blend.enabled) glEnablei(GL_BLEND, attachmentIndex); else glDisablei(GL_BLEND, attachmentIndex);
				glBlendFuncSeparatei(attachmentIndex, toGLBlendFactor(blend.sourceColor), toGLBlendFactor(blend.destinationColor), toGLBlendFactor(blend.sourceAlpha), toGLBlendFactor(blend.destinationAlpha));
				glBlendEquationSeparatei(attachmentIndex, toGLBlendOperation(blend.colorOperation), toGLBlendOperation(blend.alphaOperation));
			}
		}
		glPolygonMode(GL_FRONT_AND_BACK, descriptor.state.rasterizer.wireframe ? GL_LINE : GL_FILL);
		if (descriptor.state.rasterizer.cullMode == CullMode::None) glDisable(GL_CULL_FACE); else { glEnable(GL_CULL_FACE); glCullFace(descriptor.state.rasterizer.cullMode == CullMode::Back ? GL_BACK : GL_FRONT); }
		glFrontFace(descriptor.state.rasterizer.frontFace == FrontFace::CounterClockwise ? GL_CCW : GL_CW);
		if (descriptor.state.multisample.sampleCount > 1) glEnable(GL_MULTISAMPLE); else glDisable(GL_MULTISAMPLE);
		if (descriptor.state.multisample.sampleShading) { glEnable(GL_SAMPLE_SHADING); glMinSampleShading(descriptor.state.multisample.minimumSampleShading); } else glDisable(GL_SAMPLE_SHADING);
		if (descriptor.state.multisample.alphaToCoverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE); else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
		if (descriptor.state.multisample.alphaToOne) glEnable(GL_SAMPLE_ALPHA_TO_ONE); else glDisable(GL_SAMPLE_ALPHA_TO_ONE);
	}
	GLenum GraphicsPipeline::getGLTopology() const noexcept { switch (descriptor.state.topology) { case PrimitiveTopology::TriangleList: return GL_TRIANGLES; case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP; case PrimitiveTopology::LineList: return GL_LINES; case PrimitiveTopology::PointList: return GL_POINTS; } return GL_TRIANGLES; }
	const GraphicsPipelineDescriptor& GraphicsPipeline::getDescriptor() const noexcept { return this->descriptor; }
}
