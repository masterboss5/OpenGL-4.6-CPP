#pragma once

#include "ShaderPreprocessor.h"

#include <GL/glew.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace pipeline::device
{
class Device;
}

namespace pipeline::shader
{
class ShaderModule final
{
  public:
	struct VertexInput final
	{
		GLint Location = -1;
		GLenum Type = GL_NONE;
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

  private:
	device::Device *Device = nullptr;
	GLuint ProgramID = 0;
	ShaderStage Stage;
	std::vector<VertexInput> VertexInputs;
	std::unordered_map<std::string, GLint> UniformLocations;
};
} // namespace pipeline::shader
