#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include "ShaderTypes.h"

namespace pipeline::shader
{
	class ShaderException : public std::runtime_error
	{
	public:
		ShaderException(ShaderStage stage, std::filesystem::path path, ShaderPermutationKey permutation, std::string diagnostic);
		[[nodiscard]] ShaderStage getStage() const noexcept;
		[[nodiscard]] const std::filesystem::path& getPath() const noexcept;
		[[nodiscard]] const std::string& getDiagnostic() const noexcept;
	private:
		ShaderStage stage;
		std::filesystem::path path;
		std::string diagnostic;
	};
	class ShaderPreprocessException final : public ShaderException { public: using ShaderException::ShaderException; };
	class ShaderCompilationException final : public ShaderException { public: using ShaderException::ShaderException; };
	class ShaderLinkException final : public ShaderException { public: using ShaderException::ShaderException; };
	class ShaderInterfaceException final : public ShaderException { public: using ShaderException::ShaderException; };
	class ShaderPipelineException final : public ShaderException { public: using ShaderException::ShaderException; };
}
