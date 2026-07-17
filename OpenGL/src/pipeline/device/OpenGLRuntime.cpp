#include "OpenGLRuntime.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

#include <GL/glew.h>

namespace pipeline::device
{
	namespace
	{
		[[nodiscard]] const char* getErrorName(GLenum error)
		{
			switch (error)
			{
			case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
			case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
			case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
			case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
			case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
			case GL_STACK_UNDERFLOW: return "GL_STACK_UNDERFLOW";
			case GL_STACK_OVERFLOW: return "GL_STACK_OVERFLOW";
			default: return "GL_UNKNOWN_ERROR";
			}
		}

		[[nodiscard]] bool isEnvironmentFlagEnabled(const char* name)
		{
			char* value = nullptr;
			std::size_t valueLength = 0;
			const auto result = _dupenv_s(&value, &valueLength, name);
			if (result != 0 || value == nullptr) return false;
			const bool enabled = std::string_view(value) == "1";
			std::free(value);
			return enabled;
		}

		void GLAPIENTRY openGLDebugCallback(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*)
		{
			if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
			const char* const severityName = severity == GL_DEBUG_SEVERITY_HIGH ? "high" : severity == GL_DEBUG_SEVERITY_MEDIUM ? "medium" : "low";
			const char* const typeName = type == GL_DEBUG_TYPE_ERROR ? "error" : type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR ? "undefined behavior" : type == GL_DEBUG_TYPE_PERFORMANCE ? "performance" : "message";
			std::cerr << "OpenGL debug [" << severityName << "][" << typeName << "]: " << message << '\n';
		}
	}

	OpenGLRuntimeError::OpenGLRuntimeError(const std::string& message)
		: std::runtime_error(message)
	{
	}

	void requireOpenGL46Context()
	{
		const GLubyte* version = glGetString(GL_VERSION);
		if (version == nullptr)
		{
			throw OpenGLRuntimeError("OpenGL requires a current context");
		}

		GLint majorVersion = 0;
		GLint minorVersion = 0;
		glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
		glGetIntegerv(GL_MINOR_VERSION, &minorVersion);
		throwPendingOpenGLErrors("OpenGL context query");

		if (majorVersion < 4 || (majorVersion == 4 && minorVersion < 6))
		{
			throw OpenGLRuntimeError("This engine requires an OpenGL 4.6 context");
		}

		if (GLEW_VERSION_4_6 == GL_FALSE)
		{
			throw OpenGLRuntimeError("OpenGL 4.6 function dispatch has not been initialized");
		}
	}

	void throwPendingOpenGLErrors(std::string_view operation)
	{
		std::ostringstream message;
		bool hasError = false;
		for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError())
		{
			if (!hasError)
			{
				message << operation << " failed: ";
				hasError = true;
			}
			else
			{
				message << ", ";
			}

			message << getErrorName(error);
		}

		if (hasError)
		{
			throw OpenGLRuntimeError(message.str());
		}
	}

	void configureDebugOutput()
	{
		requireOpenGL46Context();
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(openGLDebugCallback, nullptr);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
		throwPendingOpenGLErrors("OpenGL debug output configuration");
	}

	bool isHeadlessPresentationValidationEnabled()
	{
		return isEnvironmentFlagEnabled("ENGINE_HEADLESS_VALIDATION");
	}

	bool isDeterministicRenderCoreValidationEnabled()
	{
		return isEnvironmentFlagEnabled("ENGINE_RENDER_CORE_SELF_TEST");
	}

}
