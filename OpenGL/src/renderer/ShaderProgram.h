#pragma once
#define GLEW_STATIC
#include <GL/glew.h>
#include<string>
//OLD
class ShaderProgram final {
private:
	unsigned int programID;
	unsigned int vertexShaderID;
	unsigned int fragmentShaderID;
public:
	ShaderProgram(const std::string&, const std::string&);
	~ShaderProgram() = default;

	void bind() const;
	void unbind() const;
	int getUniformLocation(const std::string&) const;
};