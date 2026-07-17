#include "ShaderSourceImporter.h"

#include "src/pipeline/shader/ShaderSourceAsset.h"

namespace resource::importer
{
	bool ShaderSourceImporter::canImport(const std::filesystem::path& path) const
	{
		const std::string extension = getNormalizedExtension(path); return extension == ".vert" || extension == ".vs" || extension == ".frag" || extension == ".fs" || extension == ".comp" || extension == ".cs";
	}
	AssetType ShaderSourceImporter::getAssetType() const noexcept { return AssetType::SHADER_SOURCE; }
	AssetImportResult ShaderSourceImporter::importCpu(const std::filesystem::path& path) const
	{
		this->validateImportRequest(path); std::vector<std::filesystem::path> includes; std::vector<AssetDependency> dependencies;
		const std::string source = this->readTextSource(path); size_t position = 0;
		while ((position = source.find("#include", position)) != std::string::npos) { const size_t first = source.find('"', position); const size_t second = first == std::string::npos ? std::string::npos : source.find('"', first + 1); if (second == std::string::npos) throw AssetContentValidationException(getAssetType(), path, "Malformed #include directive"); const std::filesystem::path include = (path.parent_path() / source.substr(first + 1, second - first - 1)).lexically_normal(); includes.push_back(include); dependencies.push_back({ .type = AssetType::SHADER_SOURCE, .path = include }); position = second + 1; }
		const std::string extension = getNormalizedExtension(path);
		const pipeline::shader::ShaderStage stage = extension == ".vert" || extension == ".vs" ? pipeline::shader::ShaderStage::Vertex : extension == ".frag" || extension == ".fs" ? pipeline::shader::ShaderStage::Fragment : pipeline::shader::ShaderStage::Compute;
		return AssetImportResult(AssetPtr<pipeline::shader::ShaderSourceAsset>::make(stage, path, source, std::move(includes)), std::move(dependencies));
	}
}
