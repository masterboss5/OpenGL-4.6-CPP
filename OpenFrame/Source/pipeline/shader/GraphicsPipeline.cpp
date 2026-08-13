#include "GraphicsPipeline.h"

#include "ShaderException.h"
#include "Source/pipeline/device/Device.h"
#include "Source/pipeline/vertex/VertexDescriptor.h"

#include <algorithm>
#include <string>

namespace pipeline::shader
{
namespace
{
constexpr std::array<string_view, static_cast<usize>(VertexUniform::Count)> VertexUniformNames{
	"shadowViewIndex", "gizmoPivot", "gizmoBasis", "gizmoScale", "gizmoOperation", "activeHandle"};
constexpr std::array<string_view, static_cast<usize>(FragmentUniform::Count)> FragmentUniformNames{"trackOverdraw", "lightCount",
																								   "clusterCount"};
constexpr std::array<GLenum, static_cast<usize>(VertexUniform::Count)> VertexUniformTypes{
	GL_UNSIGNED_INT, GL_FLOAT_VEC3, GL_FLOAT_MAT3, GL_FLOAT, GL_UNSIGNED_INT, GL_UNSIGNED_INT};
constexpr std::array<GLenum, static_cast<usize>(FragmentUniform::Count)> FragmentUniformTypes{GL_UNSIGNED_INT, GL_UNSIGNED_INT,
																							  GL_UNSIGNED_INT};

[[nodiscard]] GLint ResolveUniform(const std::unordered_map<std::string, GLint> &Locations, const string_view Name)
{
	const auto Found = Locations.find(std::string(Name));
	return Found == Locations.end() ? -1 : Found->second;
}

[[nodiscard]] bool IsSamplerType(const GLenum Type) noexcept
{
	switch (Type)
	{
	case GL_SAMPLER_1D:
	case GL_SAMPLER_2D:
	case GL_SAMPLER_3D:
	case GL_SAMPLER_CUBE:
	case GL_SAMPLER_1D_SHADOW:
	case GL_SAMPLER_2D_SHADOW:
	case GL_SAMPLER_1D_ARRAY:
	case GL_SAMPLER_2D_ARRAY:
	case GL_SAMPLER_1D_ARRAY_SHADOW:
	case GL_SAMPLER_2D_ARRAY_SHADOW:
	case GL_SAMPLER_2D_MULTISAMPLE:
	case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
	case GL_SAMPLER_CUBE_SHADOW:
	case GL_SAMPLER_BUFFER:
	case GL_SAMPLER_2D_RECT:
	case GL_SAMPLER_2D_RECT_SHADOW:
	case GL_SAMPLER_CUBE_MAP_ARRAY:
	case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
	case GL_INT_SAMPLER_1D:
	case GL_INT_SAMPLER_2D:
	case GL_INT_SAMPLER_3D:
	case GL_INT_SAMPLER_CUBE:
	case GL_INT_SAMPLER_1D_ARRAY:
	case GL_INT_SAMPLER_2D_ARRAY:
	case GL_INT_SAMPLER_2D_MULTISAMPLE:
	case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
	case GL_INT_SAMPLER_BUFFER:
	case GL_INT_SAMPLER_2D_RECT:
	case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
	case GL_UNSIGNED_INT_SAMPLER_1D:
	case GL_UNSIGNED_INT_SAMPLER_2D:
	case GL_UNSIGNED_INT_SAMPLER_3D:
	case GL_UNSIGNED_INT_SAMPLER_CUBE:
	case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
	case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
	case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
	case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
	case GL_UNSIGNED_INT_SAMPLER_BUFFER:
	case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
	case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool IsImageType(const GLenum Type) noexcept
{
	return (Type >= GL_IMAGE_1D && Type <= GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY);
}

void ValidateResourceContracts(const std::vector<ShaderResourceContract> &Contracts,
							   const std::vector<ShaderModule::UniformResource> &Resources, const ShaderStage Stage,
							   const std::filesystem::path &Path, const ShaderPermutationKey &Permutation)
{
	for (const ShaderResourceContract &Contract : Contracts)
	{
		if (Contract.Name.empty() || Contract.Type == GL_NONE || Contract.ArraySize <= 0)
			throw ShaderInterfaceException(Stage, Path, Permutation, "Shader resource contract is invalid");
		const auto Found = std::ranges::find(Resources, Contract.Name, &ShaderModule::UniformResource::Name);
		if (Found == Resources.end())
			throw ShaderInterfaceException(Stage, Path, Permutation, "Required shader resource is absent: " + Contract.Name);
		if (Found->Type != Contract.Type || Found->ArraySize != Contract.ArraySize)
			throw ShaderInterfaceException(Stage, Path, Permutation,
										   "Shader resource type or array size is incompatible: " + Contract.Name);
		const bool IsSampler = IsSamplerType(Found->Type);
		const bool IsImage = IsImageType(Found->Type);
		if ((Contract.ResourceClass == ShaderResourceClass::Uniform && (IsSampler || IsImage)) ||
			(Contract.ResourceClass == ShaderResourceClass::Sampler && !IsSampler) ||
			(Contract.ResourceClass == ShaderResourceClass::Image && !IsImage))
			throw ShaderInterfaceException(Stage, Path, Permutation, "Shader resource class is incompatible: " + Contract.Name);
		if (Contract.ResourceClass != ShaderResourceClass::Uniform && (Contract.Binding < 0 || Found->Binding != Contract.Binding))
			throw ShaderInterfaceException(Stage, Path, Permutation, "Shader resource binding is incompatible: " + Contract.Name);
	}
}

[[nodiscard]] bool MatchesVertexInput(const pipeline::vertex::VertexAttributeDescriptor &Attribute, GLenum ShaderType)
{
	const auto IsFloating = [&Attribute]()
	{
		return Attribute.Input == pipeline::vertex::VertexAttributeInput::FloatingPoint &&
			   Attribute.DataType != pipeline::vertex::VertexAttributeDataType::Float64;
	};
	const auto IsDouble = [&Attribute]()
	{
		return Attribute.Input == pipeline::vertex::VertexAttributeInput::FloatingPoint &&
			   Attribute.DataType == pipeline::vertex::VertexAttributeDataType::Float64;
	};
	const auto IsSignedInteger = [&Attribute]()
	{
		return Attribute.Input == pipeline::vertex::VertexAttributeInput::Integer &&
			   (Attribute.DataType == pipeline::vertex::VertexAttributeDataType::Int8 ||
				Attribute.DataType == pipeline::vertex::VertexAttributeDataType::Int16 ||
				Attribute.DataType == pipeline::vertex::VertexAttributeDataType::Int32);
	};
	const auto IsUnsignedInteger = [&Attribute]()
	{
		return Attribute.Input == pipeline::vertex::VertexAttributeInput::Integer &&
			   (Attribute.DataType == pipeline::vertex::VertexAttributeDataType::UInt8 ||
				Attribute.DataType == pipeline::vertex::VertexAttributeDataType::UInt16 ||
				Attribute.DataType == pipeline::vertex::VertexAttributeDataType::UInt32);
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

GraphicsPipeline::GraphicsPipeline(device::Device &Device, const GraphicsPipelineDescriptor &Descriptor,
								   std::shared_ptr<const ShaderModule> Vertex, std::shared_ptr<const ShaderModule> Fragment)
	: Device(Device), VertexProgramID(Vertex ? Vertex->GetProgramID() : 0), FragmentProgramID(Fragment ? Fragment->GetProgramID() : 0),
	  Descriptor(Descriptor), VertexModule(std::move(Vertex)), FragmentModule(std::move(Fragment)),
	  VertexInputs(this->VertexModule ? this->VertexModule->GetVertexInputs() : std::vector<ShaderModule::VertexInput>{}),
	  VertexUniformLocations(this->VertexModule ? this->VertexModule->GetUniformLocations() : std::unordered_map<std::string, GLint>{}),
	  FragmentUniformLocations(this->FragmentModule ? this->FragmentModule->GetUniformLocations()
													: std::unordered_map<std::string, GLint>{})
{
	if (!this->VertexModule || !this->FragmentModule || this->VertexModule->GetStage() != ShaderStage::Vertex ||
		this->FragmentModule->GetStage() != ShaderStage::Fragment)
		throw ShaderPipelineException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
									  "Graphics pipeline requires vertex and fragment modules");
	if ((this->VertexModule->GetEngineInterfaceMask() & Descriptor.RequiredVertexEngineInterface) !=
		Descriptor.RequiredVertexEngineInterface)
		throw ShaderInterfaceException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
									   "Vertex shader is missing a required engine UBO/SSBO binding");
	if ((this->FragmentModule->GetEngineInterfaceMask() & Descriptor.RequiredFragmentEngineInterface) !=
		Descriptor.RequiredFragmentEngineInterface)
		throw ShaderInterfaceException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									   "Fragment shader is missing a required engine UBO/SSBO binding");
	ValidateResourceContracts(Descriptor.RequiredVertexResources, this->VertexModule->GetUniformResources(), ShaderStage::Vertex,
							  Descriptor.Vertex.Path, Descriptor.Permutation);
	ValidateResourceContracts(Descriptor.RequiredFragmentResources, this->FragmentModule->GetUniformResources(), ShaderStage::Fragment,
							  Descriptor.Fragment.Path, Descriptor.Permutation);
	if (!Descriptor.State.ColorAttachmentBlends.empty() &&
		Descriptor.State.ColorAttachmentBlends.size() != Descriptor.State.RenderTargets.ColorAttachmentCount)
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "Per-attachment blend state must exactly match the render-target color attachment count");
	if (Descriptor.State.RenderTargets.ColorAttachmentCount > this->Device->GetCapabilities().MaximumDrawBuffers)
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "Graphics pipeline exceeds the device's maximum draw-buffer count");
	if (Descriptor.State.RenderTargets.ColorAttachmentCount > Descriptor.State.RenderTargets.ColorFormats.size())
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "Graphics pipeline exceeds the declared render-target format capacity");
	for (usize Index = Descriptor.State.RenderTargets.ColorAttachmentCount; Index < Descriptor.State.RenderTargets.ColorFormats.size();
		 ++Index)
	{
		if (Descriptor.State.RenderTargets.ColorFormats[Index] != GL_NONE)
			throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
										  "Render-target formats are specified beyond the pipeline color attachment count");
	}
	if (!Descriptor.State.RenderTargets.HasDepth && Descriptor.State.RenderTargets.DepthFormat != GL_NONE)
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "A depth format cannot be specified for a pipeline without a depth attachment");
	if (Descriptor.State.Multisample.SampleCount == 0 ||
		Descriptor.State.Multisample.SampleCount != Descriptor.State.RenderTargets.SampleCount)
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "Multisample state must specify the render-target sample count");
	if (Descriptor.State.Multisample.MinimumSampleShading < 0.0f || Descriptor.State.Multisample.MinimumSampleShading > 1.0f)
		throw ShaderPipelineException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
									  "Minimum sample shading must be in the [0, 1] range");
	for (usize Index = 0; Index < this->VertexUniformLocationsByID.size(); ++Index)
		this->VertexUniformLocationsByID[Index] = ResolveUniform(this->VertexUniformLocations, VertexUniformNames[Index]);
	for (usize Index = 0; Index < this->FragmentUniformLocationsByID.size(); ++Index)
		this->FragmentUniformLocationsByID[Index] = ResolveUniform(this->FragmentUniformLocations, FragmentUniformNames[Index]);
	for (usize Index = 0; Index < this->VertexUniformLocationsByID.size(); ++Index)
	{
		if ((Descriptor.RequiredVertexUniforms & (uint64{1} << Index)) != 0 && this->VertexUniformLocationsByID[Index] < 0)
			throw ShaderInterfaceException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
										   "Required vertex uniform is absent: " + string(VertexUniformNames[Index]));
		if ((Descriptor.RequiredVertexUniforms & (uint64{1} << Index)) != 0)
			ValidateResourceContracts(
				{ShaderResourceContract{.Name = string(VertexUniformNames[Index]), .Type = VertexUniformTypes[Index]}},
				this->VertexModule->GetUniformResources(), ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation);
	}
	for (usize Index = 0; Index < this->FragmentUniformLocationsByID.size(); ++Index)
	{
		if ((Descriptor.RequiredFragmentUniforms & (uint64{1} << Index)) != 0 && this->FragmentUniformLocationsByID[Index] < 0)
			throw ShaderInterfaceException(ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation,
										   "Required fragment uniform is absent: " + string(FragmentUniformNames[Index]));
		if ((Descriptor.RequiredFragmentUniforms & (uint64{1} << Index)) != 0)
			ValidateResourceContracts(
				{ShaderResourceContract{.Name = string(FragmentUniformNames[Index]), .Type = FragmentUniformTypes[Index]}},
				this->FragmentModule->GetUniformResources(), ShaderStage::Fragment, Descriptor.Fragment.Path, Descriptor.Permutation);
	}
	(void)this->Device->RequireCurrentContext();
	try
	{
		glCreateProgramPipelines(1, &this->PipelineID);
		if (this->PipelineID == 0)
			throw ShaderPipelineException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
										  "OpenGL could not allocate the separable program pipeline");
		glUseProgramStages(this->PipelineID, GL_VERTEX_SHADER_BIT, this->VertexProgramID);
		glUseProgramStages(this->PipelineID, GL_FRAGMENT_SHADER_BIT, this->FragmentProgramID);
		glValidateProgramPipeline(this->PipelineID);
		GLint Valid = GL_FALSE;
		glGetProgramPipelineiv(this->PipelineID, GL_VALIDATE_STATUS, &Valid);
		if (Valid != GL_TRUE)
			throw ShaderPipelineException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
										  "OpenGL rejected the separable program pipeline");
		this->Device->CheckErrors("GraphicsPipeline creation");
	}
	catch (...)
	{
		if (this->PipelineID != 0)
			glDeleteProgramPipelines(1, &this->PipelineID);
		this->PipelineID = 0;
		throw;
	}
}
void GraphicsPipeline::SetVertexUniformUInt(const VertexUniform Uniform, const uint32 Value) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->VertexUniformLocationsByID.size() || this->VertexUniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Vertex, this->Descriptor.Vertex.Path, this->Descriptor.Permutation,
									   "Required typed vertex uniform is absent from the realized pipeline");
	glProgramUniform1ui(this->VertexProgramID, this->VertexUniformLocationsByID[Index], Value);
}

void GraphicsPipeline::SetFragmentUniformUInt(const FragmentUniform Uniform, const uint32 Value) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->FragmentUniformLocationsByID.size() || this->FragmentUniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Fragment, this->Descriptor.Fragment.Path, this->Descriptor.Permutation,
									   "Required typed fragment uniform is absent from the realized pipeline");
	glProgramUniform1ui(this->FragmentProgramID, this->FragmentUniformLocationsByID[Index], Value);
}

void GraphicsPipeline::SetVertexUniformFloat(const VertexUniform Uniform, const float32 Value) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->VertexUniformLocationsByID.size() || this->VertexUniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Vertex, this->Descriptor.Vertex.Path, this->Descriptor.Permutation,
									   "Required typed vertex uniform is absent from the realized pipeline");
	glProgramUniform1f(this->VertexProgramID, this->VertexUniformLocationsByID[Index], Value);
}

void GraphicsPipeline::SetVertexUniformFloat3(const VertexUniform Uniform, const glm::vec3 &Value) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->VertexUniformLocationsByID.size() || this->VertexUniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Vertex, this->Descriptor.Vertex.Path, this->Descriptor.Permutation,
									   "Required typed vertex uniform is absent from the realized pipeline");
	glProgramUniform3fv(this->VertexProgramID, this->VertexUniformLocationsByID[Index], 1, &Value.x);
}

void GraphicsPipeline::SetVertexUniformMatrix3(const VertexUniform Uniform, const glm::mat3 &Value) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->VertexUniformLocationsByID.size() || this->VertexUniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Vertex, this->Descriptor.Vertex.Path, this->Descriptor.Permutation,
									   "Required typed vertex uniform is absent from the realized pipeline");
	glProgramUniformMatrix3fv(this->VertexProgramID, this->VertexUniformLocationsByID[Index], 1, GL_FALSE, &Value[0][0]);
}

void GraphicsPipeline::ValidateVertexDescriptor(const pipeline::vertex::VertexDescriptor &VertexDescriptor) const
{
	const uint64 LayoutHash = VertexDescriptor.GetLayoutHash();
	const auto CachedLayout = std::find(this->ValidatedVertexLayouts.begin(),
										this->ValidatedVertexLayouts.begin() + this->ValidatedVertexLayoutCount, LayoutHash);
	if (CachedLayout != this->ValidatedVertexLayouts.begin() + this->ValidatedVertexLayoutCount)
		return;
	for (const ShaderModule::VertexInput &Input : this->VertexInputs)
	{
		const auto Attributes = VertexDescriptor.GetAttributes();
		const auto Attribute =
			std::find_if(Attributes.begin(), Attributes.end(), [&Input](const pipeline::vertex::VertexAttributeDescriptor &Candidate)
						 { return Candidate.Location == static_cast<GLuint>(Input.Location); });
		if (Attribute == Attributes.end() || !MatchesVertexInput(*Attribute, Input.Type))
		{
			throw ShaderInterfaceException(ShaderStage::Vertex, Descriptor.Vertex.Path, Descriptor.Permutation,
										   "VertexDescriptor is incompatible with required vertex input location " +
											   std::to_string(Input.Location));
		}
	}
	if (this->ValidatedVertexLayoutCount < this->ValidatedVertexLayouts.size())
		this->ValidatedVertexLayouts[this->ValidatedVertexLayoutCount++] = LayoutHash;
}
GraphicsPipeline::~GraphicsPipeline()
{
	if (this->PipelineID != 0)
	{
		if (device::Device *LiveDevice = this->Device.TryGet(); LiveDevice != nullptr)
			LiveDevice->RetireGPUObject(device::GPUObjectType::ProgramPipeline, this->PipelineID);
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
	this->Device->ApplyGraphicsPipelineState(this->Descriptor.State);
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
