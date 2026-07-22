#pragma once

#include "ShaderModule.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace pipeline::shader
{
struct ComputePipelineDescriptor final
{
	ShaderSourceDescriptor Compute;
	ShaderPermutationKey Permutation;
};

class ComputePipeline final
{
  public:
	ComputePipeline(device::Device &Device, const ComputePipelineDescriptor &Descriptor, const ShaderModule &Compute);
	void Bind() const;
	// Parameters are resolved once per pipeline rather than from render-pass
	// execution.  A missing optional parameter is intentionally a no-op.
	void SetUniformUInt(string_view Name, uint32 Value) const;
	void SetUniformUInt2(string_view Name, uint32 X, uint32 Y) const;
	[[nodiscard]] GLuint GetProgramID() const noexcept;
	[[nodiscard]] const ComputePipelineDescriptor &GetDescriptor() const noexcept;

  private:
	device::Device *Device = nullptr;
	ComputePipelineDescriptor Descriptor;
	GLuint ProgramID = 0;
	std::unordered_map<std::string, GLint> UniformLocations;
};
} // namespace pipeline::shader
