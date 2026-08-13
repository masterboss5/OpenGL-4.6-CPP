#include "ComputePipeline.h"

#include "ShaderException.h"
#include "Source/pipeline/device/Device.h"

#include <array>
#include <algorithm>
#include <string_view>

namespace pipeline::shader
{
namespace
{
constexpr std::array<string_view, static_cast<usize>(ComputeUniform::Count)> ComputeUniformNames{
	"candidateCount",	  "pyramidMipCount", "historyValid", "scratchCapacity", "batchCount",	"blockCount",
	"sourceExtent",		  "sourceMip",		 "sourceScale",	 "lightCount",		"clusterCount", "viewMode",
	"selectionWordCount", "outlineRadius",	 "outlineColor", "operation",		"deltaSeconds"};
constexpr std::array<GLenum, static_cast<usize>(ComputeUniform::Count)> ComputeUniformTypes{
	GL_UNSIGNED_INT,	  GL_UNSIGNED_INT, GL_UNSIGNED_INT, GL_UNSIGNED_INT, GL_UNSIGNED_INT, GL_UNSIGNED_INT,
	GL_UNSIGNED_INT_VEC2, GL_UNSIGNED_INT, GL_UNSIGNED_INT, GL_UNSIGNED_INT, GL_UNSIGNED_INT, GL_UNSIGNED_INT,
	GL_UNSIGNED_INT,	  GL_UNSIGNED_INT, GL_FLOAT_VEC4,	GL_UNSIGNED_INT, GL_FLOAT};

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
	return Type >= GL_IMAGE_1D && Type <= GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY;
}

void ValidateResourceContracts(const std::vector<ShaderResourceContract> &Contracts,
							   const std::vector<ShaderModule::UniformResource> &Resources, const std::filesystem::path &Path,
							   const ShaderPermutationKey &Permutation)
{
	for (const ShaderResourceContract &Contract : Contracts)
	{
		if (Contract.Name.empty() || Contract.Type == GL_NONE || Contract.ArraySize <= 0)
			throw ShaderInterfaceException(ShaderStage::Compute, Path, Permutation, "Shader resource contract is invalid");
		const auto Found = std::ranges::find(Resources, Contract.Name, &ShaderModule::UniformResource::Name);
		if (Found == Resources.end() || Found->Type != Contract.Type || Found->ArraySize != Contract.ArraySize)
			throw ShaderInterfaceException(ShaderStage::Compute, Path, Permutation,
										   "Required shader resource is absent or incompatible: " + Contract.Name);
		const bool IsSampler = IsSamplerType(Found->Type);
		const bool IsImage = IsImageType(Found->Type);
		if ((Contract.ResourceClass == ShaderResourceClass::Uniform && (IsSampler || IsImage)) ||
			(Contract.ResourceClass == ShaderResourceClass::Sampler && !IsSampler) ||
			(Contract.ResourceClass == ShaderResourceClass::Image && !IsImage) ||
			(Contract.ResourceClass != ShaderResourceClass::Uniform && (Contract.Binding < 0 || Found->Binding != Contract.Binding)))
			throw ShaderInterfaceException(ShaderStage::Compute, Path, Permutation,
										   "Shader resource contract is incompatible: " + Contract.Name);
	}
}
} // namespace

ComputePipeline::ComputePipeline(device::Device &Device, const ComputePipelineDescriptor &Descriptor,
								 std::shared_ptr<const ShaderModule> Compute)
	: Device(Device), Descriptor(Descriptor), ProgramID(Compute ? Compute->GetProgramID() : 0), ComputeModule(std::move(Compute)),
	  UniformLocations(this->ComputeModule ? this->ComputeModule->GetUniformLocations() : std::unordered_map<std::string, GLint>{})
{
	if (!this->ComputeModule || this->ComputeModule->GetStage() != ShaderStage::Compute)
	{
		throw ShaderPipelineException(ShaderStage::Compute, Descriptor.Compute.Path, Descriptor.Permutation,
									  "Compute pipeline requires a compute shader module");
	}
	if ((this->ComputeModule->GetEngineInterfaceMask() & Descriptor.RequiredEngineInterface) != Descriptor.RequiredEngineInterface)
		throw ShaderInterfaceException(ShaderStage::Compute, Descriptor.Compute.Path, Descriptor.Permutation,
									   "Compute shader is missing a required engine UBO/SSBO binding");
	ValidateResourceContracts(Descriptor.RequiredResources, this->ComputeModule->GetUniformResources(), Descriptor.Compute.Path,
							  Descriptor.Permutation);
	for (usize Index = 0; Index < this->UniformLocationsByID.size(); ++Index)
		this->UniformLocationsByID[Index] = ResolveUniform(this->UniformLocations, ComputeUniformNames[Index]);
	for (usize Index = 0; Index < this->UniformLocationsByID.size(); ++Index)
	{
		if ((Descriptor.RequiredUniforms & (uint64{1} << Index)) != 0 && this->UniformLocationsByID[Index] < 0)
		{
			throw ShaderInterfaceException(ShaderStage::Compute, Descriptor.Compute.Path, Descriptor.Permutation,
										   "Required compute uniform is absent: " + string(ComputeUniformNames[Index]));
		}
		if ((Descriptor.RequiredUniforms & (uint64{1} << Index)) != 0)
			ValidateResourceContracts(
				{ShaderResourceContract{.Name = string(ComputeUniformNames[Index]), .Type = ComputeUniformTypes[Index]}},
				this->ComputeModule->GetUniformResources(), Descriptor.Compute.Path, Descriptor.Permutation);
	}
}

void ComputePipeline::Bind() const
{
	(void)this->Device->RequireCurrentContext();
	// Compute uses a monolithic program. Clear any separable graphics
	// pipeline so dispatch never inherits stale graphics state.
	glBindProgramPipeline(0);
	glUseProgram(this->ProgramID);
}

void ComputePipeline::SetUniformUInt(const ComputeUniform Uniform, const uint32 Value) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->UniformLocationsByID.size() || this->UniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Compute, this->Descriptor.Compute.Path, this->Descriptor.Permutation,
									   "Required typed compute uniform is absent from the realized pipeline");
	glProgramUniform1ui(this->ProgramID, this->UniformLocationsByID[Index], Value);
}

void ComputePipeline::SetUniformUInt2(const ComputeUniform Uniform, const uint32 X, const uint32 Y) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->UniformLocationsByID.size() || this->UniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Compute, this->Descriptor.Compute.Path, this->Descriptor.Permutation,
									   "Required typed compute uniform is absent from the realized pipeline");
	glProgramUniform2ui(this->ProgramID, this->UniformLocationsByID[Index], X, Y);
}

void ComputePipeline::SetUniformFloat4(const ComputeUniform Uniform, const float32 X, const float32 Y, const float32 Z,
									   const float32 W) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->UniformLocationsByID.size() || this->UniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Compute, this->Descriptor.Compute.Path, this->Descriptor.Permutation,
									   "Required typed compute uniform is absent from the realized pipeline");
	glProgramUniform4f(this->ProgramID, this->UniformLocationsByID[Index], X, Y, Z, W);
}

void ComputePipeline::SetUniformFloat(const ComputeUniform Uniform, const float32 Value) const
{
	(void)this->Device->RequireCurrentContext();
	const usize Index = static_cast<usize>(Uniform);
	if (Index >= this->UniformLocationsByID.size() || this->UniformLocationsByID[Index] < 0)
		throw ShaderInterfaceException(ShaderStage::Compute, this->Descriptor.Compute.Path, this->Descriptor.Permutation,
									   "Required typed compute uniform is absent from the realized pipeline");
	glProgramUniform1f(this->ProgramID, this->UniformLocationsByID[Index], Value);
}

GLuint ComputePipeline::GetProgramID() const noexcept
{
	return this->ProgramID;
}

const ComputePipelineDescriptor &ComputePipeline::GetDescriptor() const noexcept
{
	return this->Descriptor;
}
} // namespace pipeline::shader
