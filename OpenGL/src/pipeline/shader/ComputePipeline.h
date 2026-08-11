#pragma once

#include "src/core/EngineAPI.h"

#include "ShaderModule.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pipeline::shader
{
struct ComputePipelineDescriptor final
{
	ShaderSourceDescriptor Compute;
	ShaderPermutationKey Permutation;
	uint64 RequiredUniforms = 0;
	uint64 RequiredEngineInterface = 0;
	std::vector<ShaderResourceContract> RequiredResources;
};

enum class ComputeUniform : uint8
{
	CandidateCount,
	PyramidMipCount,
	HistoryValid,
	ScratchCapacity,
	BatchCount,
	BlockCount,
	SourceExtent,
	SourceMip,
	SourceScale,
	LightCount,
	ClusterCount,
	ViewMode,
	SelectionWordCount,
	OutlineRadius,
	OutlineColor,
	Operation,
	DeltaSeconds,
	Count
};

[[nodiscard]] constexpr uint64 UniformBit(const ComputeUniform Uniform) noexcept
{
	return uint64{1} << static_cast<uint8>(Uniform);
}

class ENGINE_API ComputePipeline final
{
  public:
	ComputePipeline(device::Device &Device, const ComputePipelineDescriptor &Descriptor, std::shared_ptr<const ShaderModule> Compute);
	void Bind() const;
	void SetUniformUInt(ComputeUniform Uniform, uint32 Value) const;
	void SetUniformUInt2(ComputeUniform Uniform, uint32 X, uint32 Y) const;
	void SetUniformFloat4(ComputeUniform Uniform, float32 X, float32 Y, float32 Z, float32 W) const;
	void SetUniformFloat(ComputeUniform Uniform, float32 Value) const;
	[[nodiscard]] GLuint GetProgramID() const noexcept;
	[[nodiscard]] const ComputePipelineDescriptor &GetDescriptor() const noexcept;

  private:
	device::DeviceHandle Device;
	ComputePipelineDescriptor Descriptor;
	GLuint ProgramID = 0;
	std::shared_ptr<const ShaderModule> ComputeModule;
	std::unordered_map<std::string, GLint> UniformLocations;
	std::array<GLint, static_cast<usize>(ComputeUniform::Count)> UniformLocationsByID{};
};
} // namespace pipeline::shader
