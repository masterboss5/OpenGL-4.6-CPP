#include "ShaderException.h"

namespace pipeline::shader
{
	ShaderException::ShaderException(ShaderStage stage, std::filesystem::path path, ShaderPermutationKey permutation, std::string diagnostic)
		: std::runtime_error(path.string() + " [permutation=" + permutation.toString() + "]: " + diagnostic), stage(stage), path(std::move(path)), diagnostic(std::move(diagnostic)) {}
	ShaderStage ShaderException::getStage() const noexcept { return this->stage; }
	const std::filesystem::path& ShaderException::getPath() const noexcept { return this->path; }
	const std::string& ShaderException::getDiagnostic() const noexcept { return this->diagnostic; }
}
