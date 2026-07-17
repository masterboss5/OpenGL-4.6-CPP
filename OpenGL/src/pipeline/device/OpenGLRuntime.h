#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace pipeline::device
{
	class OpenGLRuntimeError final : public std::runtime_error
	{
	public:
		explicit OpenGLRuntimeError(const std::string& message);
	};

	// Requires a current context and initialized GLEW dispatch for this engine's OpenGL 4.6 baseline.
	void requireOpenGL46Context();

	// Converts all pending OpenGL errors into one diagnostic exception at an API boundary.
	void throwPendingOpenGLErrors(std::string_view operation);

	// Enables core OpenGL debug output for the current OpenGL 4.6 context. The
	// callback is diagnostic-only: API-boundary error conversion remains the
	// deterministic failure path.
	void configureDebugOutput();

	// Opt-in test mode used by automated presentation validation. Normal engine
	// runs never set this flag and therefore keep their regular visible window.
	[[nodiscard]] bool isHeadlessPresentationValidationEnabled();
	[[nodiscard]] bool isDeterministicRenderCoreValidationEnabled();
}
