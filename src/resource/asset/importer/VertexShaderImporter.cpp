#include "VertexShaderImporter.h"

bool resource::importer::VertexShaderImporter::canImport(const std::filesystem::path& path)
{
    bool extension = path.extension().string() == ".vert";

    return extension;
}

resource::AssetType resource::importer::VertexShaderImporter::getAssetType() const
{
    return resource::AssetType::VERTEX_SHADER;
}

resource::Asset* resource::importer::VertexShaderImporter::import(const std::filesystem::path& path)
{
    pipeline::shader::VertexShaderSource source {};
    source.sourcePath = path;
    source.source = file::readFile(path).source;

	if (source.source.empty())
    {
		LOG_ERROR("Failed to read vertex shader for: ");
        return nullptr;
    }

	return new pipeline::shader::VertexShader(source);
}
