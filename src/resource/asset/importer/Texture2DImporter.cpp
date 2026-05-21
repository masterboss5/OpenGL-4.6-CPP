#include "Texture2DImporter.h"

bool resource::importer::Texture2DImporter::canImport(const std::filesystem::path& path)
{
    bool extension = path.extension().string() == ".png";
    return extension;
}

resource::AssetType resource::importer::Texture2DImporter::getAssetType() const
{
    return resource::AssetType::TEXTURE_2D;
}

resource::Asset* resource::importer::Texture2DImporter::import(const std::filesystem::path& path)
{
    renderer::texture::Texture2DCreationInfo source;
	uint8* pixels = stbi_load(path.string().c_str(), &source.width, &source.height, &source.channels, 4);
	source.pixels = pixels;

    if (source.pixels == nullptr)
    {
        LOG_ERROR("Failed to load 2D texture from path: " + path.string());
		return nullptr;
    }

	renderer::texture::Texture2D* texture = new renderer::texture::Texture2D(path.filename().string(), source);
	stbi_image_free(pixels);

    return texture;
}
