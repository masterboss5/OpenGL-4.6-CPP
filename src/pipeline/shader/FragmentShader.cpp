#include "FragmentShader.h"

pipeline::shader::FragmentShader::FragmentShader(FragmentShaderSource& source)
	: Asset(util::UUID::generateRandomUUID()),
	source(source)
{
	this->ID = glCreateShader(GL_FRAGMENT_SHADER);
}

pipeline::shader::FragmentShader::~FragmentShader()
{
	LOG_INFO("Deleting fragment shader for path: " + this->source.sourcePath.string());
	glDeleteShader(this->ID);
}

void pipeline::shader::FragmentShader::compile()
{
    if (!this->isCompiled())
    {
		const GLchar* source = this->source.source.c_str();
        glShaderSource(this->ID, 1, &source, nullptr);
		glCompileShader(this->ID);
		GLint success;
		glGetShaderiv(this->ID, GL_COMPILE_STATUS, &success);

        if (!success)
        {
			GLchar infoLog[OGL_GLSL_INFO_LOGGING_SIZE];
			glGetShaderInfoLog(this->ID, OGL_GLSL_INFO_LOGGING_SIZE, nullptr, infoLog);
            LOG_ERROR("Failed to compile fragment shader for path: " + this->source.sourcePath.string());
        }
		else
		{
			this->compiled = true;
		}
    }
}

bool pipeline::shader::FragmentShader::isCompiled() const
{
    return this->compiled;
}

const pipeline::shader::FragmentShaderSource& pipeline::shader::FragmentShader::getSource() const
{
	return this->source;
}
