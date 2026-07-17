#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <GL/glew.h>

#include "ShaderModule.h"

namespace renderer
{
	class VertexDescriptor;
}

namespace pipeline::shader
{
	enum class PrimitiveTopology : uint8 { TriangleList, TriangleStrip, LineList, PointList };
	enum class CullMode : uint8 { None, Back, Front };
	enum class FrontFace : uint8 { CounterClockwise, Clockwise };
	enum class CompareFunction : uint8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
	enum class BlendFactor : uint8 { Zero, One, SourceColor, OneMinusSourceColor, DestinationColor, OneMinusDestinationColor, SourceAlpha, OneMinusSourceAlpha, DestinationAlpha, OneMinusDestinationAlpha };
	enum class BlendOperation : uint8 { Add, Subtract, ReverseSubtract, Minimum, Maximum };

	struct RasterizerState final { CullMode cullMode = CullMode::Back; FrontFace frontFace = FrontFace::CounterClockwise; bool wireframe = false; };
	struct DepthStencilState final { bool depthTest = true; bool depthWrite = true; CompareFunction depthCompare = CompareFunction::Greater; };
	struct BlendState final { bool enabled = false; BlendFactor sourceColor = BlendFactor::One; BlendFactor destinationColor = BlendFactor::Zero; BlendFactor sourceAlpha = BlendFactor::One; BlendFactor destinationAlpha = BlendFactor::Zero; BlendOperation colorOperation = BlendOperation::Add; BlendOperation alphaOperation = BlendOperation::Add; };
	struct MultisampleState final { uint8 sampleCount = 1; bool sampleShading = false; float32 minimumSampleShading = 1.0f; bool alphaToCoverage = false; bool alphaToOne = false; };
	struct RenderTargetSignature final { uint8 colorAttachmentCount = 1; bool hasDepth = true; uint8 sampleCount = 1; };
	// When populated, colorAttachmentBlends is indexed by fragment output /
	// render-target attachment. This is required for MRT techniques such as
	// weighted blended OIT, whose accumulation and revealage targets use
	// different blend equations.
	struct GraphicsPipelineState final { RasterizerState rasterizer; DepthStencilState depthStencil; BlendState blend; std::vector<BlendState> colorAttachmentBlends; MultisampleState multisample; PrimitiveTopology topology = PrimitiveTopology::TriangleList; RenderTargetSignature renderTargets; };
	struct GraphicsPipelineDescriptor final { ShaderSourceDescriptor vertex; ShaderSourceDescriptor fragment; ShaderPermutationKey permutation; GraphicsPipelineState state; };

	class GraphicsPipeline final
	{
	public:
		GraphicsPipeline(const GraphicsPipelineDescriptor& descriptor, const ShaderModule& vertex, const ShaderModule& fragment);
		~GraphicsPipeline();
		GraphicsPipeline(const GraphicsPipeline&) = delete;
		void bind() const;
		void setVertexUniformUInt(string_view name, uint32 value) const;
		void validateVertexDescriptor(const renderer::VertexDescriptor& vertexDescriptor) const;
		[[nodiscard]] GLenum getGLTopology() const noexcept;
		[[nodiscard]] const GraphicsPipelineDescriptor& getDescriptor() const noexcept;
	private:
		GLuint pipelineID = 0;
		GLuint vertexProgramID = 0;
		GraphicsPipelineDescriptor descriptor;
		std::vector<ShaderModule::VertexInput> vertexInputs;
		mutable std::unordered_map<std::string, GLint> vertexUniformLocations;
		mutable std::unordered_set<uint64> validatedVertexLayouts;
	};
}
