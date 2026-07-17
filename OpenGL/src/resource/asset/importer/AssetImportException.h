#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include "src/resource/asset/AssetTypes.h"

namespace resource::importer
{
	class AssetImportException : public std::runtime_error
	{
	public:
		AssetImportException(AssetType assetType, std::filesystem::path sourcePath, std::string diagnostic);

		[[nodiscard]] AssetType getAssetType() const noexcept;
		[[nodiscard]] const std::filesystem::path& getSourcePath() const noexcept;
		[[nodiscard]] const std::string& getDiagnostic() const noexcept;

	private:
		AssetType assetType;
		std::filesystem::path sourcePath;
		std::string diagnostic;
	};

	class AssetNotFoundException final : public AssetImportException
	{
	public:
		AssetNotFoundException(AssetType assetType, const std::filesystem::path& sourcePath);
	};

	class AssetUnsupportedFormatException final : public AssetImportException
	{
	public:
		AssetUnsupportedFormatException(AssetType assetType, const std::filesystem::path& sourcePath);
	};

	class AssetFileReadException final : public AssetImportException
	{
	public:
		AssetFileReadException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic);
	};

	class AssetImageDecodeException final : public AssetImportException
	{
	public:
		AssetImageDecodeException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic);
	};

	class AssetModelParseException final : public AssetImportException
	{
	public:
		AssetModelParseException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic);
	};

	class AssetContentValidationException final : public AssetImportException
	{
	public:
		AssetContentValidationException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic);
	};

	class AssetImporterNotRegisteredException final : public AssetImportException
	{
	public:
		AssetImporterNotRegisteredException(AssetType assetType, const std::filesystem::path& sourcePath);
	};

	class AssetUnexpectedImportException final : public AssetImportException
	{
	public:
		AssetUnexpectedImportException(AssetType assetType, const std::filesystem::path& sourcePath, std::string diagnostic);
	};
}
