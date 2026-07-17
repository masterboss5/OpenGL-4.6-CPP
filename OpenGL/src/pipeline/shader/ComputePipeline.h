#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "ShaderModule.h"

namespace pipeline::shader
{
	struct ComputePipelineDescriptor final
	{
		ShaderSourceDescriptor compute;
		ShaderPermutationKey permutation;
	};

	class ComputePipeline final
	{
	public:
		ComputePipeline(const ComputePipelineDescriptor& descriptor, const ShaderModule& compute);
		void bind() const;
		// Parameters are resolved once per pipeline rather than from render-pass
		// execution.  A missing optional parameter is intentionally a no-op.
		void setUniformUInt(string_view name, uint32 value) const;
		void setUniformUInt2(string_view name, uint32 x, uint32 y) const;
		[[nodiscard]] GLuint getProgramID() const noexcept;
		[[nodiscard]] const ComputePipelineDescriptor& getDescriptor() const noexcept;
	private:
		ComputePipelineDescriptor descriptor;
		GLuint programID = 0;
		mutable std::unordered_map<std::string, GLint> uniformLocations;
	};
}
