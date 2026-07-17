#include "AssetImporter.h"

#include <locale>

namespace resource::importer
{
	AssetImportResult::AssetImportResult(AssetPtr<Asset> asset, std::vector<AssetDependency> dependencies)
		: asset(std::move(asset)),
		dependencies(std::move(dependencies))
	{
		if (this->asset == nullptr)
		{
			throw std::invalid_argument("AssetImportResult requires a valid imported asset");
		}
	}

	void AssetImporter::validateImportRequest(const std::filesystem::path& path) const
	{
		std::error_code error;
		const bool exists = std::filesystem::exists(path, error);
		if (error)
		{
			throw AssetFileReadException(this->getAssetType(), path, error.message());
		}
		if (!exists)
		{
			throw AssetNotFoundException(this->getAssetType(), path);
		}
		if (!std::filesystem::is_regular_file(path, error) || error)
		{
			throw AssetFileReadException(this->getAssetType(), path, error ? error.message() : "Source path is not a regular file");
		}
		if (!this->canImport(path))
		{
			throw AssetUnsupportedFormatException(this->getAssetType(), path);
		}

		std::ifstream accessProbe(path, std::ios::in | std::ios::binary);
		if (!accessProbe.is_open())
		{
			throw AssetFileReadException(this->getAssetType(), path, "Unable to open source file");
		}
	}

	std::string AssetImporter::readTextSource(const std::filesystem::path& path) const
	{
		std::ifstream input(path, std::ios::in | std::ios::binary);
		if (!input.is_open())
		{
			throw AssetFileReadException(this->getAssetType(), path, "Unable to open source file");
		}

		std::ostringstream buffer;
		buffer << input.rdbuf();
		if (input.bad())
		{
			throw AssetFileReadException(this->getAssetType(), path, "I/O failure while reading source file");
		}

		std::string source = buffer.str();
		if (source.empty())
		{
			throw AssetContentValidationException(this->getAssetType(), path, "Source file is empty");
		}
		return source;
	}

	std::string AssetImporter::getNormalizedExtension(const std::filesystem::path& path)
	{
		std::string extension = path.extension().string();
		const std::locale locale = std::locale::classic();
		for (auto& character : extension)
		{
			character = std::tolower(character, locale);
		}
		return extension;
	}
}
