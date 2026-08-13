#pragma once

#include "Source/core/EngineAPI.h"

#include "ShaderModule.h"

#include <GL/glew.h>
#include <array>
#include <glm.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pipeline::vertex
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
enum class StencilOperation : uint8
{
	Keep,
	Zero,
	Replace,
	IncrementClamp,
	DecrementClamp,
	Invert,
	IncrementWrap,
	DecrementWrap
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
struct StencilFaceState final
{
	CompareFunction Compare = CompareFunction::Always;
	StencilOperation Fail = StencilOperation::Keep;
	StencilOperation DepthFail = StencilOperation::Keep;
	StencilOperation Pass = StencilOperation::Keep;
	uint32 Reference = 0;
	uint8 ReadMask = 0xFF;
	uint8 WriteMask = 0xFF;
};
struct DepthStencilState final
{
	bool DepthTest = true;
	bool DepthWrite = true;
	CompareFunction DepthCompare = CompareFunction::Greater;
	bool StencilTest = false;
	StencilFaceState FrontStencil;
	StencilFaceState BackStencil;
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
	// GL_NONE keeps the format unconstrained for reusable pipelines. When a
	// format is supplied, the render graph validates it against the pass
	// attachment before the pipeline executes.
	std::array<GLenum, 8> ColorFormats{};
	GLenum DepthFormat = GL_NONE;
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
	uint64 RequiredVertexUniforms = 0;
	uint64 RequiredFragmentUniforms = 0;
	uint64 RequiredVertexEngineInterface = 0;
	uint64 RequiredFragmentEngineInterface = 0;
	std::vector<ShaderResourceContract> RequiredVertexResources;
	std::vector<ShaderResourceContract> RequiredFragmentResources;
};

enum class VertexUniform : uint8
{
	ShadowViewIndex,
	GizmoPivot,
	GizmoBasis,
	GizmoScale,
	GizmoOperation,
	ActiveHandle,
	Count
};

enum class FragmentUniform : uint8
{
	TrackOverdraw,
	LightCount,
	ClusterCount,
	Count
};

[[nodiscard]] constexpr uint64 UniformBit(const VertexUniform Uniform) noexcept
{
	return uint64{1} << static_cast<uint8>(Uniform);
}

[[nodiscard]] constexpr uint64 UniformBit(const FragmentUniform Uniform) noexcept
{
	return uint64{1} << static_cast<uint8>(Uniform);
}

class ENGINE_API GraphicsPipeline final
{
  public:
	GraphicsPipeline(device::Device &Device, const GraphicsPipelineDescriptor &Descriptor, std::shared_ptr<const ShaderModule> Vertex,
					 std::shared_ptr<const ShaderModule> Fragment);
	~GraphicsPipeline();
	GraphicsPipeline(const GraphicsPipeline &) = delete;
	void Bind() const;
	void SetVertexUniformUInt(VertexUniform Uniform, uint32 Value) const;
	void SetFragmentUniformUInt(FragmentUniform Uniform, uint32 Value) const;
	void SetVertexUniformFloat(VertexUniform Uniform, float32 Value) const;
	void SetVertexUniformFloat3(VertexUniform Uniform, const glm::vec3 &Value) const;
	void SetVertexUniformMatrix3(VertexUniform Uniform, const glm::mat3 &Value) const;
	void ValidateVertexDescriptor(const pipeline::vertex::VertexDescriptor &VertexDescriptor) const;
	[[nodiscard]] GLenum GetGLTopology() const noexcept;
	[[nodiscard]] const GraphicsPipelineDescriptor &GetDescriptor() const noexcept;

  private:
	device::DeviceHandle Device;
	GLuint PipelineID = 0;
	GLuint VertexProgramID = 0;
	GLuint FragmentProgramID = 0;
	GraphicsPipelineDescriptor Descriptor;
	std::shared_ptr<const ShaderModule> VertexModule;
	std::shared_ptr<const ShaderModule> FragmentModule;
	std::vector<ShaderModule::VertexInput> VertexInputs;
	std::unordered_map<std::string, GLint> VertexUniformLocations;
	std::unordered_map<std::string, GLint> FragmentUniformLocations;
	std::array<GLint, static_cast<usize>(VertexUniform::Count)> VertexUniformLocationsByID{};
	std::array<GLint, static_cast<usize>(FragmentUniform::Count)> FragmentUniformLocationsByID{};
	// The common case uses this small fixed cache. If a pipeline is used with
	// more layouts than fit, the additional layouts are revalidated without
	// allocating from the draw path.
	static constexpr usize VertexLayoutCacheCapacity = 64;
	mutable std::array<uint64, VertexLayoutCacheCapacity> ValidatedVertexLayouts{};
	mutable usize ValidatedVertexLayoutCount = 0;
};
} // namespace pipeline::shader
