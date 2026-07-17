#pragma once

#include <vector>

#include <GL/glew.h>

#include "ShaderPreprocessor.h"

namespace pipeline::shader
{
	class ShaderModule final
	{
	public:
		struct VertexInput final
		{
			GLint location = -1;
			GLenum type = GL_NONE;
		};

		ShaderModule(const ShaderSourceAsset& source, ShaderPermutationKey permutation, const ShaderPreprocessResult& preprocessed);
		~ShaderModule();
		ShaderModule(const ShaderModule&) = delete;
		[[nodiscard]] GLuint getProgramID() const noexcept;
		[[nodiscard]] ShaderStage getStage() const noexcept;
		// Program-interface reflection is performed once after linking. Graphics
		// pipelines use this immutable cache instead of querying OpenGL at draw time.
		[[nodiscard]] const std::vector<VertexInput>& getVertexInputs() const noexcept;
	private:
		GLuint programID = 0;
		ShaderStage stage;
		std::vector<VertexInput> vertexInputs;
	};
}
