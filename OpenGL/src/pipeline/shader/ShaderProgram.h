#pragma once
#include <GL/glew.h>
#include "src/resource/Asset.h"

namespace pipeline::shader
{
	class ShaderProgram final : public resource::Asset
	{
	private:

		GLuint ID;
		bool linked = false;

	public:

		ShaderProgram();
		~ShaderProgram();
		ShaderProgram(const ShaderProgram&) = delete;
		ShaderProgram& operator=(const ShaderProgram&) = delete;

		ShaderProgram(ShaderProgram&&) noexcept;
		ShaderProgram& operator=(ShaderProgram&&) noexcept;

		void attachShader(GLuint shaderID);
	};
}