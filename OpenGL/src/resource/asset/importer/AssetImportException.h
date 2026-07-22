#pragma once

#include "src/resource/asset/AssetTypes.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace resource::importer
{
class AssetImportException : public std::runtime_error
{
  public:
	AssetImportException(resource::AssetType Type, std::filesystem::path SourcePath, std::string Diagnostic);

	[[nodiscard]] AssetType GetAssetType() const noexcept;
	[[nodiscard]] const std::filesystem::path &GetSourcePath() const noexcept;
	[[nodiscard]] const std::string &GetDiagnostic() const noexcept;

  private:
	resource::AssetType Type;
	std::filesystem::path SourcePath;
	std::string Diagnostic;
};

class AssetNotFoundException final : public AssetImportException
{
  public:
	AssetNotFoundException(resource::AssetType Type, const std::filesystem::path &SourcePath);
};

class AssetUnsupportedFormatException final : public AssetImportException
{
  public:
	AssetUnsupportedFormatException(resource::AssetType Type, const std::filesystem::path &SourcePath);
};

class AssetFileReadException final : public AssetImportException
{
  public:
	AssetFileReadException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class AssetImageDecodeException final : public AssetImportException
{
  public:
	AssetImageDecodeException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class AssetModelParseException final : public AssetImportException
{
  public:
	AssetModelParseException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class AssetContentValidationException final : public AssetImportException
{
  public:
	AssetContentValidationException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class AssetImporterNotRegisteredException final : public AssetImportException
{
  public:
	AssetImporterNotRegisteredException(resource::AssetType Type, const std::filesystem::path &SourcePath);
};

class AssetUnexpectedImportException final : public AssetImportException
{
  public:
	AssetUnexpectedImportException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};
} // namespace resource::importer
