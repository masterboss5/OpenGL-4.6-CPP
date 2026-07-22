#include "ShaderSourceImporter.h"

#include "src/pipeline/shader/ShaderSourceAsset.h"

namespace resource::importer
{
bool ShaderSourceImporter::CanImport(const std::filesystem::path &Path) const
{
	const std::string Extension = GetNormalizedExtension(Path);
	return Extension == ".vert" || Extension == ".vs" || Extension == ".frag" || Extension == ".fs" || Extension == ".comp" ||
		   Extension == ".cs";
}
AssetType ShaderSourceImporter::GetAssetType() const noexcept
{
	return AssetType::ShaderSource;
}
AssetImportResult ShaderSourceImporter::ImportCPU(const std::filesystem::path &Path, AssetImportContext &Context) const
{
	(void)Context;
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
		const std::filesystem::path Include = (Path.parent_path() / Source.substr(First + 1, Second - First - 1)).lexically_normal();
		Includes.push_back(Include);
		Dependencies.push_back({.Type = AssetType::ShaderSource, .Path = Include});
		Position = Second + 1;
	}
	const std::string Extension = GetNormalizedExtension(Path);
	const pipeline::shader::ShaderStage Stage = Extension == ".vert" || Extension == ".vs"	 ? pipeline::shader::ShaderStage::Vertex
												: Extension == ".frag" || Extension == ".fs" ? pipeline::shader::ShaderStage::Fragment
																							 : pipeline::shader::ShaderStage::Compute;
	return AssetImportResult(AssetPtr<pipeline::shader::ShaderSourceAsset>::Make(Stage, Path, Source, std::move(Includes)),
							 std::move(Dependencies));
}
} // namespace resource::importer
