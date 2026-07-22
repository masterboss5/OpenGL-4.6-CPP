#include "AssetImporter.h"

#include <locale>

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

	std::ifstream AccessProbe(Path, std::ios::in | std::ios::binary);
	if (!AccessProbe.is_open())
	{
		throw AssetFileReadException(this->GetAssetType(), Path, "Unable to open source file");
	}
}

std::string AssetImporter::ReadTextSource(const std::filesystem::path &Path) const
{
	std::ifstream Input(Path, std::ios::in | std::ios::binary);
	if (!Input.is_open())
	{
		throw AssetFileReadException(this->GetAssetType(), Path, "Unable to open source file");
	}

	std::ostringstream Buffer;
	Buffer << Input.rdbuf();
	if (Input.bad())
	{
		throw AssetFileReadException(this->GetAssetType(), Path, "I/O failure while reading source file");
	}

	std::string Source = Buffer.str();
	if (Source.empty())
	{
		throw AssetContentValidationException(this->GetAssetType(), Path, "Source file is empty");
	}
	return Source;
}

std::string AssetImporter::GetNormalizedExtension(const std::filesystem::path &Path)
{
	std::string Extension = Path.extension().string();
	const std::locale Locale = std::locale::classic();
	for (auto &Character : Extension)
	{
		Character = std::tolower(Character, Locale);
	}
	return Extension;
}
} // namespace resource::importer
