#include "AssetImportException.h"

namespace resource::importer
{
namespace
{
std::string BuildMessage(const std::filesystem::path &SourcePath, const std::string &Diagnostic)
{
	return SourcePath.string() + ": " + Diagnostic;
}
} // namespace

AssetImportException::AssetImportException(resource::AssetType Type, std::filesystem::path SourcePath, std::string Diagnostic)
	: std::runtime_error(BuildMessage(SourcePath, Diagnostic)), Type(Type), SourcePath(std::move(SourcePath)),
	  Diagnostic(std::move(Diagnostic))
{
}

AssetType AssetImportException::GetAssetType() const noexcept
{
	return this->Type;
}

const std::filesystem::path &AssetImportException::GetSourcePath() const noexcept
{
	return this->SourcePath;
}

const std::string &AssetImportException::GetDiagnostic() const noexcept
{
	return this->Diagnostic;
}

AssetNotFoundException::AssetNotFoundException(resource::AssetType Type, const std::filesystem::path &SourcePath)
	: AssetImportException(Type, SourcePath, "Asset source file does not exist")
{
}

AssetUnsupportedFormatException::AssetUnsupportedFormatException(resource::AssetType Type, const std::filesystem::path &SourcePath)
	: AssetImportException(Type, SourcePath, "Asset importer does not support this file format")
{
}

AssetFileReadException::AssetFileReadException(resource::AssetType Type, const std::filesystem::path &SourcePath, std::string Diagnostic)
	: AssetImportException(Type, SourcePath, "Unable to read asset source: " + Diagnostic)
{
}

AssetImageDecodeException::AssetImageDecodeException(resource::AssetType Type, const std::filesystem::path &SourcePath,
													 std::string Diagnostic)
	: AssetImportException(Type, SourcePath, "Unable to decode image: " + Diagnostic)
{
}

AssetModelParseException::AssetModelParseException(resource::AssetType Type, const std::filesystem::path &SourcePath,
												   std::string Diagnostic)
	: AssetImportException(Type, SourcePath, "Unable to parse model: " + Diagnostic)
{
}

AssetContentValidationException::AssetContentValidationException(resource::AssetType Type, const std::filesystem::path &SourcePath,
																 std::string Diagnostic)
	: AssetImportException(Type, SourcePath, "Imported asset content is invalid: " + Diagnostic)
{
}

AssetImporterNotRegisteredException::AssetImporterNotRegisteredException(resource::AssetType Type, const std::filesystem::path &SourcePath)
	: AssetImportException(Type, SourcePath, "No importer is registered for this asset type")
{
}

AssetUnexpectedImportException::AssetUnexpectedImportException(resource::AssetType Type, const std::filesystem::path &SourcePath,
															   std::string Diagnostic)
	: AssetImportException(Type, SourcePath, "Unexpected importer failure: " + Diagnostic)
{
}
} // namespace resource::importer
