#include "ComputePipeline.h"

#include "ShaderException.h"

namespace pipeline::shader
{
	ComputePipeline::ComputePipeline(const ComputePipelineDescriptor& descriptor, const ShaderModule& compute)
		: descriptor(descriptor), programID(compute.getProgramID())
	{
		if (compute.getStage() != ShaderStage::Compute)
		{
			throw ShaderPipelineException(ShaderStage::Compute, descriptor.compute.path, descriptor.permutation, "Compute pipeline requires a compute shader module");
		}
	}

	void ComputePipeline::bind() const
	{
		// Compute uses a monolithic program. Clear any separable graphics
		// pipeline so dispatch never inherits stale graphics state.
		glBindProgramPipeline(0);
		glUseProgram(this->programID);
	}

	void ComputePipeline::setUniformUInt(string_view name, uint32 value) const
	{
		const std::string uniformName(name);
		const auto [locationIt, inserted] = uniformLocations.try_emplace(uniformName, -1);
		if (inserted)
		{
			locationIt->second = glGetUniformLocation(this->programID, uniformName.c_str());
		}
		if (locationIt->second >= 0)
		{
			glProgramUniform1ui(this->programID, locationIt->second, value);
		}
	}

	void ComputePipeline::setUniformUInt2(string_view name, uint32 x, uint32 y) const
	{
		const std::string uniformName(name);
		const auto [locationIt, inserted] = uniformLocations.try_emplace(uniformName, -1);
		if (inserted)
		{
			locationIt->second = glGetUniformLocation(this->programID, uniformName.c_str());
		}
		if (locationIt->second >= 0)
		{
			glProgramUniform2ui(this->programID, locationIt->second, x, y);
		}
	}

	GLuint ComputePipeline::getProgramID() const noexcept
	{
		return this->programID;
	}

	const ComputePipelineDescriptor& ComputePipeline::getDescriptor() const noexcept
	{
		return this->descriptor;
	}
}
