#include "ShaderSourceImporter.h"

#include "Source/pipeline/shader/ShaderSourceAsset.h"

namespace resource::importer
{
bool ShaderSourceImporter::CanImport(const std::filesystem::path &Path) const
{
	const std::string Extension = GetNormalizedExtension(Path);
	return Extension == ".vert" || Extension == ".vs" || Extension == ".frag" || Extension == ".fs" || Extension == ".comp" ||
		   Extension == ".cs" || Extension == ".glsl" || Extension == ".inc";
}
AssetType ShaderSourceImporter::GetAssetType() const noexcept
{
	return AssetType::ShaderSource;
}
AssetImportResult ShaderSourceImporter::ImportCPU(const std::filesystem::path &Path, AssetImportContext &Context) const
{
	this->ValidateImportRequest(Path);
	std::vector<std::filesystem::path> Includes;
	std::vector<AssetDependency> Dependencies;
	const std::string Source = this->ReadTextSource(Path);
	usize Position = 0;
	while ((Position = Source.find("#include", Position)) != std::string::npos)
	{
		const usize First = Source.find('"', Position);
		const usize Second = First == std::string::npos ? std::string::npos : Source.find('"', First + 1);
		if (Second == std::string::npos)
			throw AssetContentValidationException(GetAssetType(), Path, "Malformed #include directive");
		const std::filesystem::path Include = Context.ResolveDependencyPath(
			AssetType::ShaderSource, Path, Source.substr(First + 1, Second - First - 1), "Shader include dependency");
		Includes.push_back(Include);
		Dependencies.push_back({.Type = AssetType::ShaderSource, .Path = Include});
		Position = Second + 1;
	}
	const std::string Extension = GetNormalizedExtension(Path);
	const pipeline::shader::ShaderStage Stage = Extension == ".vert" || Extension == ".vs"	 ? pipeline::shader::ShaderStage::Vertex
												: Extension == ".frag" || Extension == ".fs" ? pipeline::shader::ShaderStage::Fragment
												: Extension == ".comp" || Extension == ".cs" ? pipeline::shader::ShaderStage::Compute
																							 : pipeline::shader::ShaderStage::Include;
	return AssetImportResult(AssetPtr<pipeline::shader::ShaderSourceAsset>::Make(Stage, Path, Source, std::move(Includes)),
							 std::move(Dependencies));
}
} // namespace resource::importer
