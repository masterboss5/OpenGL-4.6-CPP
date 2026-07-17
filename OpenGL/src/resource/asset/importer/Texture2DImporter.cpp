#include "Texture2DImporter.h"

#include <limits>
#include <memory>

#include "stb_image.h"
#include "src/resource/asset/Texture2DAsset.h"

bool resource::importer::Texture2DImporter::canImport(const std::filesystem::path& path) const
{
	const std::string extension = getNormalizedExtension(path);
	return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga";
}

resource::AssetType resource::importer::Texture2DImporter::getAssetType() const noexcept
{
	return AssetType::TEXTURE_2D;
}

resource::importer::AssetImportResult resource::importer::Texture2DImporter::importCpu(const std::filesystem::path& path) const
{
	this->validateImportRequest(path);

	int32 width = 0;
	int32 height = 0;
	int32 channels = 0;
	stbi_uc* sourcePixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
	if (sourcePixels == nullptr)
	{
		const auto* failureReason = stbi_failure_reason();
		throw AssetImageDecodeException(AssetType::TEXTURE_2D, path, failureReason == nullptr ? "Image decoder did not provide a diagnostic" : failureReason);
	}
	std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> ownedPixels(sourcePixels, stbi_image_free);

	if (width <= 0 || height <= 0)
	{
		throw AssetContentValidationException(AssetType::TEXTURE_2D, path, "Decoded image has invalid dimensions");
	}

	const size_t widthInBytes = static_cast<size_t>(width);
	const size_t heightInBytes = static_cast<size_t>(height);
	constexpr size_t rgbaChannelCount = 4;
	if (widthInBytes > std::numeric_limits<size_t>::max() / heightInBytes / rgbaChannelCount)
	{
		throw AssetContentValidationException(AssetType::TEXTURE_2D, path, "Decoded image dimensions overflow the CPU pixel buffer size");
	}

	const size_t byteCount = widthInBytes * heightInBytes * rgbaChannelCount;
	std::vector<uint8> pixels(ownedPixels.get(), ownedPixels.get() + byteCount);

	return AssetImportResult(AssetPtr<Texture2DAsset>::make(path.filename().string(), width, height, static_cast<int32>(rgbaChannelCount), std::move(pixels)));
}
