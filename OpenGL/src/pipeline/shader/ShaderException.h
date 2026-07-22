#pragma once

#include "ShaderTypes.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace pipeline::shader
{
class ShaderException : public std::runtime_error
{
  public:
	ShaderException(ShaderStage Stage, std::filesystem::path Path, ShaderPermutationKey Permutation, std::string Diagnostic);
	[[nodiscard]] ShaderStage GetStage() const noexcept;
	[[nodiscard]] const std::filesystem::path &GetPath() const noexcept;
	[[nodiscard]] const std::string &GetDiagnostic() const noexcept;

  private:
	ShaderStage Stage;
	std::filesystem::path Path;
	std::string Diagnostic;
};
class ShaderPreprocessException final : public ShaderException
{
  public:
	using ShaderException::ShaderException;
};
class ShaderCompilationException final : public ShaderException
{
  public:
	using ShaderException::ShaderException;
};
class ShaderLinkException final : public ShaderException
{
  public:
	using ShaderException::ShaderException;
};
class ShaderInterfaceException final : public ShaderException
{
  public:
	using ShaderException::ShaderException;
};
class ShaderPipelineException final : public ShaderException
{
  public:
	using ShaderException::ShaderException;
};
} // namespace pipeline::shader
