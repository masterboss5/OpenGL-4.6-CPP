#include "ShaderException.h"

namespace pipeline::shader
{
ShaderException::ShaderException(ShaderStage Stage, std::filesystem::path Path, ShaderPermutationKey Permutation, std::string Diagnostic)
	: std::runtime_error(Path.string() + " [permutation=" + Permutation.ToString() + "]: " + Diagnostic), Stage(Stage),
	  Path(std::move(Path)), Diagnostic(std::move(Diagnostic))
{
}
ShaderStage ShaderException::GetStage() const noexcept
{
	return this->Stage;
}
const std::filesystem::path &ShaderException::GetPath() const noexcept
{
	return this->Path;
}
const std::string &ShaderException::GetDiagnostic() const noexcept
{
	return this->Diagnostic;
}
} // namespace pipeline::shader
