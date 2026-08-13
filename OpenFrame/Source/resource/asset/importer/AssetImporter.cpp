#include "AssetImporter.h"

#include "Source/core/io/SecurePath.h"

#include <cctype>
#include <limits>

namespace resource::importer
{
AssetImportResult::AssetImportResult(AssetPtr<resource::Asset> Asset, std::vector<AssetDependency> Dependencies)
	: Asset(std::move(Asset)), Dependencies(std::move(Dependencies))
{
	if (this->Asset == nullptr)
	{
		throw std::invalid_argument("AssetImportResult requires a valid imported asset");
	}
}

void AssetImporter::ValidateImportRequest(const std::filesystem::path &Path) const
{
	std::error_code Error;
	const bool Exists = std::filesystem::exists(Path, Error);
	if (Error)
	{
		throw AssetFileReadException(this->GetAssetType(), Path, Error.message());
	}
	if (!Exists)
	{
		throw AssetNotFoundException(this->GetAssetType(), Path);
	}
	if (!std::filesystem::is_regular_file(Path, Error) || Error)
	{
		throw AssetFileReadException(this->GetAssetType(), Path, Error ? Error.message() : "Source path is not a regular file");
	}
	if (!this->CanImport(Path))
	{
		throw AssetUnsupportedFormatException(this->GetAssetType(), Path);
	}

	try
	{
		(void)core::io::SecurePath::ReadFileRangeWithin(Path.parent_path(), Path.filename(), 0, 0, std::numeric_limits<uint64>::max(),
														"Asset import source");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw AssetFileReadException(this->GetAssetType(), Path, Exception.what());
	}
}

std::string AssetImporter::ReadTextSource(const std::filesystem::path &Path) const
{
	constexpr uint64 MaximumTextAssetBytes = 64U * 1024U * 1024U;
	std::vector<uint8> Bytes;
	try
	{
		Bytes = core::io::SecurePath::ReadFileWithin(Path.parent_path(), Path.filename(), MaximumTextAssetBytes, "Text asset source");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw AssetFileReadException(this->GetAssetType(), Path, Exception.what());
	}
	std::string Source(Bytes.begin(), Bytes.end());
	if (Source.empty())
	{
		throw AssetContentValidationException(this->GetAssetType(), Path, "Source file is empty");
	}
	return Source;
}

std::string AssetImporter::GetNormalizedExtension(const std::filesystem::path &Path)
{
	std::string Extension = Path.extension().string();
	for (auto &Character : Extension)
		Character = static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
	return Extension;
}
} // namespace resource::importer
