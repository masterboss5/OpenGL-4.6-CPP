#include "AssetImportException.h"

namespace resource::importer
{
	namespace
	{
		std::string buildMessage(const std::filesystem::path& sourcePath, const std::string& diagnostic)
		{
			return sourcePath.string() + ": " + diagnostic;
		}
	}

	AssetImportException::AssetImportException(AssetType assetType, std::filesystem::path sourcePath, std::string diagnostic)
		: std::runtime_error(buildMessage(sourcePath, diagnostic)),
		assetType(assetType),
		sourcePath(std::move(sourcePath)),
		diagnostic(std::move(diagnostic))
	{
	}

	AssetType AssetImportException::getAssetType() const noexcept
	{
		return this->assetType;
	}

	const std::filesystem::path& AssetImportException::getSourcePath() const noexcept
	{
		return this->sourcePath;
	}

	const std::string& AssetImportException::getDiagnostic() const noexcept
	{
		return this->diagnostic;
	}

	AssetNotFoundException::AssetNotFoundException(AssetType assetType, const std::filesystem::path& sourcePath)
		: AssetImportException(assetType, sourcePath, "Asset source file does not exist")
	{
	}

	AssetUnsupportedFormatException::AssetUnsupportedFormatException(AssetType assetType, const std::filesystem::path& sourcePath)
		: AssetImportException(assetType, sourcePath, "Asset importer does not support this file format")
	{
	}

	AssetFileReadException::AssetFileReadException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic)
		: AssetImportException(assetType, sourcePath, "Unable to read asset source: " + diagnostic)
	{
	}

	AssetImageDecodeException::AssetImageDecodeException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic)
		: AssetImportException(assetType, sourcePath, "Unable to decode image: " + diagnostic)
	{
	}

	AssetModelParseException::AssetModelParseException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic)
		: AssetImportException(assetType, sourcePath, "Unable to parse model: " + diagnostic)
	{
	}

	AssetContentValidationException::AssetContentValidationException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic)
		: AssetImportException(assetType, sourcePath, "Imported asset content is invalid: " + diagnostic)
	{
	}

	AssetImporterNotRegisteredException::AssetImporterNotRegisteredException(AssetType assetType, const std::filesystem::path& sourcePath)
		: AssetImportException(assetType, sourcePath, "No importer is registered for this asset type")
	{
	}

	AssetUnexpectedImportException::AssetUnexpectedImportException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic)
		: AssetImportException(assetType, sourcePath, "Unexpected importer failure: " + diagnostic)
	{
	}
}
