#pragma once

#include "Source/core/EngineAPI.h"
#include "Source/pipeline/device/Device.h"

#include "ShaderPreprocessor.h"

#include <GL/glew.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace pipeline::shader
{
enum class ShaderResourceClass : uint8
{
	Uniform,
	Sampler,
	Image
};

struct ShaderResourceContract final
{
	std::string Name;
	ShaderResourceClass ResourceClass = ShaderResourceClass::Uniform;
	GLenum Type = GL_NONE;
	GLint Binding = -1;
	GLint ArraySize = 1;
};

class ENGINE_API ShaderModule final
{
  public:
	struct VertexInput final
	{
		GLint Location = -1;
		GLenum Type = GL_NONE;
	};
	struct UniformResource final
	{
		std::string Name;
		GLint Location = -1;
		GLenum Type = GL_NONE;
		GLint ArraySize = 0;
		// For opaque resources this is the texture/image unit established by
		// the shader's layout(binding=...) declaration. Ordinary uniforms use -1.
		GLint Binding = -1;
	};

	ShaderModule(device::Device &Device, const ShaderSourceAsset &Source, ShaderPermutationKey Permutation,
				 const ShaderPreprocessResult &Preprocessed);
	~ShaderModule();
	ShaderModule(const ShaderModule &) = delete;
	[[nodiscard]] GLuint GetProgramID() const noexcept;
	[[nodiscard]] ShaderStage GetStage() const noexcept;
	// Program-interface reflection is performed once after linking. Graphics
	// pipelines use this immutable cache instead of querying OpenGL at draw time.
	[[nodiscard]] const std::vector<VertexInput> &GetVertexInputs() const noexcept;
	[[nodiscard]] const std::unordered_map<std::string, GLint> &GetUniformLocations() const noexcept;
	[[nodiscard]] const std::vector<UniformResource> &GetUniformResources() const noexcept;
	[[nodiscard]] uint64 GetEngineInterfaceMask() const noexcept;
	[[nodiscard]] static uint64 GetInterfaceQueryCountForValidation() noexcept;

  private:
	device::DeviceHandle Device;
	GLuint ProgramID = 0;
	ShaderStage Stage;
	std::vector<VertexInput> VertexInputs;
	std::unordered_map<std::string, GLint> UniformLocations;
	std::vector<UniformResource> UniformResources;
	uint64 EngineInterfaceMask = 0;
};

[[nodiscard]] constexpr uint64 UniformBlockBindingBit(const uint32 Binding) noexcept
{
	return Binding < 16U ? uint64{1} << Binding : uint64{0};
}

[[nodiscard]] constexpr uint64 StorageBlockBindingBit(const uint32 Binding) noexcept
{
	return Binding < 16U ? uint64{1} << (Binding + 16U) : uint64{0};
}
} // namespace pipeline::shader
