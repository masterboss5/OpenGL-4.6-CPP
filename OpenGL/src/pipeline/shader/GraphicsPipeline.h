#pragma once

#include "ShaderModule.h"

#include <GL/glew.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace renderer
{
class VertexDescriptor;
}

namespace pipeline::shader
{
enum class PrimitiveTopology : uint8
{
	TriangleList,
	TriangleStrip,
	LineList,
	PointList
};
enum class CullMode : uint8
{
	None,
	Back,
	Front
};
enum class FrontFace : uint8
{
	CounterClockwise,
	Clockwise
};
enum class CompareFunction : uint8
{
	Never,
	Less,
	Equal,
	LessEqual,
	Greater,
	NotEqual,
	GreaterEqual,
	Always
};
enum class BlendFactor : uint8
{
	Zero,
	One,
	SourceColor,
	OneMinusSourceColor,
	DestinationColor,
	OneMinusDestinationColor,
	SourceAlpha,
	OneMinusSourceAlpha,
	DestinationAlpha,
	OneMinusDestinationAlpha
};
enum class BlendOperation : uint8
{
	Add,
	Subtract,
	ReverseSubtract,
	Minimum,
	Maximum
};

struct RasterizerState final
{
	CullMode CullMode = CullMode::Back;
	FrontFace FrontFace = FrontFace::CounterClockwise;
	bool Wireframe = false;
};
struct DepthStencilState final
{
	bool DepthTest = true;
	bool DepthWrite = true;
	CompareFunction DepthCompare = CompareFunction::Greater;
};
struct BlendState final
{
	bool Enabled = false;
	BlendFactor SourceColor = BlendFactor::One;
	BlendFactor DestinationColor = BlendFactor::Zero;
	BlendFactor SourceAlpha = BlendFactor::One;
	BlendFactor DestinationAlpha = BlendFactor::Zero;
	BlendOperation ColorOperation = BlendOperation::Add;
	BlendOperation AlphaOperation = BlendOperation::Add;
};
struct MultisampleState final
{
	uint8 SampleCount = 1;
	bool SampleShading = false;
	float32 MinimumSampleShading = 1.0f;
	bool AlphaToCoverage = false;
	bool AlphaToOne = false;
};
struct RenderTargetSignature final
{
	uint8 ColorAttachmentCount = 1;
	bool HasDepth = true;
	uint8 SampleCount = 1;
};
// When populated, colorAttachmentBlends is indexed by fragment output /
// render-target attachment. This is required for MRT techniques such as
// weighted blended OIT, whose accumulation and revealage targets use
// different blend equations.
struct GraphicsPipelineState final
{
	RasterizerState Rasterizer;
	DepthStencilState DepthStencil;
	BlendState Blend;
	std::vector<BlendState> ColorAttachmentBlends;
	MultisampleState Multisample;
	PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
	RenderTargetSignature RenderTargets;
};
struct GraphicsPipelineDescriptor final
{
	ShaderSourceDescriptor Vertex;
	ShaderSourceDescriptor Fragment;
	ShaderPermutationKey Permutation;
	GraphicsPipelineState State;
};

class GraphicsPipeline final
{
  public:
	GraphicsPipeline(device::Device &Device, const GraphicsPipelineDescriptor &Descriptor, const ShaderModule &Vertex,
					 const ShaderModule &Fragment);
	~GraphicsPipeline();
	GraphicsPipeline(const GraphicsPipeline &) = delete;
	void Bind() const;
	void SetVertexUniformUInt(string_view Name, uint32 Value) const;
	void ValidateVertexDescriptor(const renderer::VertexDescriptor &VertexDescriptor) const;
	[[nodiscard]] GLenum GetGLTopology() const noexcept;
	[[nodiscard]] const GraphicsPipelineDescriptor &GetDescriptor() const noexcept;

  private:
	device::Device *Device = nullptr;
	GLuint PipelineID = 0;
	GLuint VertexProgramID = 0;
	GraphicsPipelineDescriptor Descriptor;
	std::vector<ShaderModule::VertexInput> VertexInputs;
	std::unordered_map<std::string, GLint> VertexUniformLocations;
	mutable std::unordered_set<uint64> ValidatedVertexLayouts;
};
} // namespace pipeline::shader
