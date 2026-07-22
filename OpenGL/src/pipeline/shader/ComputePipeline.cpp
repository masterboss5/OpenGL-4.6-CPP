#include "ComputePipeline.h"

#include "ShaderException.h"
#include "src/pipeline/device/Device.h"

namespace pipeline::shader
{
ComputePipeline::ComputePipeline(device::Device &Device, const ComputePipelineDescriptor &Descriptor, const ShaderModule &Compute)
	: Device(&Device), Descriptor(Descriptor), ProgramID(Compute.GetProgramID()), UniformLocations(Compute.GetUniformLocations())
{
	if (Compute.GetStage() != ShaderStage::Compute)
	{
		throw ShaderPipelineException(ShaderStage::Compute, Descriptor.Compute.Path, Descriptor.Permutation,
									  "Compute pipeline requires a compute shader module");
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

void ComputePipeline::SetUniformUInt(string_view Name, uint32 Value) const
{
	(void)this->Device->RequireCurrentContext();
	const auto LocationIt = this->UniformLocations.find(std::string(Name));
	if (LocationIt != this->UniformLocations.end())
	{
		glProgramUniform1ui(this->ProgramID, LocationIt->second, Value);
	}
}

void ComputePipeline::SetUniformUInt2(string_view Name, uint32 X, uint32 Y) const
{
	(void)this->Device->RequireCurrentContext();
	const auto LocationIt = this->UniformLocations.find(std::string(Name));
	if (LocationIt != this->UniformLocations.end())
	{
		glProgramUniform2ui(this->ProgramID, LocationIt->second, X, Y);
	}
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
