#pragma once

#include "src/resource/asset/AssetTypes.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace resource::importer
{
class ENGINE_API AssetImportException : public std::runtime_error
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

class ENGINE_API AssetNotFoundException final : public AssetImportException
{
  public:
	AssetNotFoundException(resource::AssetType Type, const std::filesystem::path &SourcePath);
};

class ENGINE_API AssetUnsupportedFormatException final : public AssetImportException
{
  public:
	AssetUnsupportedFormatException(resource::AssetType Type, const std::filesystem::path &SourcePath);
};

class ENGINE_API AssetFileReadException final : public AssetImportException
{
  public:
	AssetFileReadException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class ENGINE_API AssetImageDecodeException final : public AssetImportException
{
  public:
	AssetImageDecodeException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class ENGINE_API AssetModelParseException final : public AssetImportException
{
  public:
	AssetModelParseException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class ENGINE_API AssetMaterialParseException final : public AssetImportException
{
  public:
	AssetMaterialParseException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class ENGINE_API AssetContentValidationException final : public AssetImportException
{
  public:
	AssetContentValidationException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class ENGINE_API AssetImporterNotRegisteredException final : public AssetImportException
{
  public:
	AssetImporterNotRegisteredException(resource::AssetType Type, const std::filesystem::path &SourcePath);
};

class ENGINE_API AssetUnexpectedImportException final : public AssetImportException
{
  public:
	AssetUnexpectedImportException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};

class ENGINE_API AssetDependencyCycleException final : public AssetImportException
{
  public:
	AssetDependencyCycleException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic);
};
} // namespace resource::importer
