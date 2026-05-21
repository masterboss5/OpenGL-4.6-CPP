#include "VertexShader.h"

pipeline::shader::VertexShader::VertexShader(VertexShaderSource& source)
	: Asset(util::UUID::generateRandomUUID()),
    source(source)
{
    this->ID = glCreateShader(GL_VERTEX_SHADER);
}

pipeline::shader::VertexShader::~VertexShader()
{
	LOG_INFO("Deleting vertex shader for path: " + this->source.sourcePath.string());
	glDeleteShader(this->ID);
}

void pipeline::shader::VertexShader::compile()
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
            LOG_ERROR("Failed to compile vertex shader for path: " + this->source.sourcePath.string());
        }
        else
        {
			this->compiled = true;
        }
    }
}

bool pipeline::shader::VertexShader::isCompiled() const
{
    return this->compiled;
}

const pipeline::shader::VertexShaderSource& pipeline::shader::VertexShader::getSource() const
{
	return this->source;
}
