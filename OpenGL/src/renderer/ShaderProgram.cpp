#include "ShaderProgram.h"
#include "src/util/files.h"
#include<iostream>

void checkCompileErrors(GLuint shader, std::string type) {
	GLint success;
	GLchar infoLog[1024];
	if (type != "PROGRAM") {
		glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
		if (!success) {
			glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
			std::cerr << type << " SHADER COMPILATION ERROR:\n" << infoLog << "\n";
		}
	}
	else {
		glGetProgramiv(shader, GL_LINK_STATUS, &success);
		if (!success) {
			glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
			std::cerr << "PROGRAM LINKING ERROR:\n" << infoLog << "\n";
		}
	}
}

ShaderProgram::ShaderProgram(const std::string& vertexPath, const std::string& fragmentPath) {
	std::string vertexCode = file::readFile(vertexPath).source;
	std::string fragmentCode = file::readFile(fragmentPath).source;

	this->vertexShaderID = glCreateShader(GL_VERTEX_SHADER);
	const char* vertexSource = vertexCode.c_str();
	glShaderSource(this->vertexShaderID, 1, &vertexSource, nullptr);
	glCompileShader(this->vertexShaderID);

	this->fragmentShaderID = glCreateShader(GL_FRAGMENT_SHADER);
	const char* fragmentSource = fragmentCode.c_str();
	glShaderSource(this->fragmentShaderID, 1, &fragmentSource, nullptr);
	glCompileShader(this->fragmentShaderID);

	this->programID = glCreateProgram();
	glAttachShader(this->programID, this->vertexShaderID);
	glAttachShader(this->programID, this->fragmentShaderID);
	glLinkProgram(this->programID);

	checkCompileErrors(this->vertexShaderID, "VERTEX");
	checkCompileErrors(this->fragmentShaderID, "FRAGMENT");
	checkCompileErrors(this->programID, "PROGRAM");
}

void ShaderProgram::bind() const {
	glUseProgram(this->programID);
}

void ShaderProgram::unbind() const {
	glUseProgram(0);
}

int ShaderProgram::getUniformLocation(const std::string& location) const {
	return glGetUniformLocation(this->programID, location.c_str());
}