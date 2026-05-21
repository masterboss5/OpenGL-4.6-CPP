#include "FragmentShaderImporter.h"

bool resource::importer::FragmentShaderImporter::canImport(const std::filesystem::path& path)
{
    bool extension = path.extension().string() == ".frag";

    return extension;
}

resource::AssetType resource::importer::FragmentShaderImporter::getAssetType() const
{
    return resource::AssetType::FRAGMENT_SHADER;
}

resource::Asset* resource::importer::FragmentShaderImporter::import(const std::filesystem::path& path)
{
    pipeline::shader::FragmentShaderSource source{};
    source.sourcePath = path;
    source.source = file::readFile(path).source;

    if (source.source.empty())
    {
        LOG_ERROR("Failed to read fragment shader for: ");
        return nullptr;
    }

    return new pipeline::shader::FragmentShader(source);
}