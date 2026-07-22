#include "Texture2DImporter.h"

#include "src/resource/asset/Texture2DAsset.h"
#include "stb_image.h"

#include <limits>
#include <memory>

bool resource::importer::Texture2DImporter::CanImport(const std::filesystem::path &Path) const
{
	const std::string Extension = GetNormalizedExtension(Path);
	return Extension == ".png" || Extension == ".jpg" || Extension == ".jpeg" || Extension == ".tga";
}

resource::AssetType resource::importer::Texture2DImporter::GetAssetType() const noexcept
{
	return AssetType::Texture2D;
}

resource::importer::AssetImportResult resource::importer::Texture2DImporter::ImportCPU(const std::filesystem::path &Path,
																					   AssetImportContext &Context) const
{
	(void)Context;
	this->ValidateImportRequest(Path);

	int32 Width = 0;
	int32 Height = 0;
	int32 Channels = 0;
	stbi_uc *SourcePixels = stbi_load(Path.string().c_str(), &Width, &Height, &Channels, STBI_rgb_alpha);
	if (SourcePixels == nullptr)
	{
		const auto *FailureReason = stbi_failure_reason();
		throw AssetImageDecodeException(AssetType::Texture2D, Path,
										FailureReason == nullptr ? "Image decoder did not provide a diagnostic" : FailureReason);
	}
	std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> OwnedPixels(SourcePixels, stbi_image_free);

	if (Width <= 0 || Height <= 0)
	{
		throw AssetContentValidationException(AssetType::Texture2D, Path, "Decoded image has invalid dimensions");
	}

	const usize WidthInBytes = static_cast<usize>(Width);
	const usize HeightInBytes = static_cast<usize>(Height);
	constexpr usize RGBAChannelCount = 4;
	if (WidthInBytes > std::numeric_limits<usize>::max() / HeightInBytes / RGBAChannelCount)
	{
		throw AssetContentValidationException(AssetType::Texture2D, Path, "Decoded image dimensions overflow the CPU pixel buffer size");
	}

	const usize ByteCount = WidthInBytes * HeightInBytes * RGBAChannelCount;
	std::vector<uint8> Pixels(OwnedPixels.get(), OwnedPixels.get() + ByteCount);

	return AssetImportResult(
		AssetPtr<Texture2DAsset>::Make(Path.filename().string(), Width, Height, static_cast<int32>(RGBAChannelCount), std::move(Pixels)));
}
