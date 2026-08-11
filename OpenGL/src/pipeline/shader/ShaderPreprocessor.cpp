#include "ShaderPreprocessor.h"

#include "ShaderException.h"
#include "src/core/io/SecurePath.h"

#include <cctype>
#include <sstream>
#include <unordered_set>

namespace pipeline::shader
{
namespace
{
constexpr uint64 MaximumShaderSourceBytes = 64U * 1024U * 1024U;
constexpr usize MaximumShaderIncludeFiles = 1'024U;
constexpr uint32 MaximumShaderIncludeDepth = 64U;

std::string ReadSource(const std::filesystem::path &Root, const std::filesystem::path &Path, ShaderStage Stage,
					   const ShaderPermutationKey &Key)
{
	try
	{
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(Root, Path.lexically_relative(Root), MaximumShaderSourceBytes, "Shader include");
		return std::string(Bytes.begin(), Bytes.end());
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw ShaderPreprocessException(Stage, Path, Key, "Unable to securely read included source: " + string(Exception.what()));
	}
}
bool IncludePath(const std::string &Line, std::string &Output)
{
	const usize Include = Line.find("#include");
	if (Include == std::string::npos)
		return false;
	const usize FirstQuote = Line.find('"', Include);
	const usize SecondQuote = FirstQuote == std::string::npos ? std::string::npos : Line.find('"', FirstQuote + 1);
	if (FirstQuote == std::string::npos || SecondQuote == std::string::npos)
		return false;
	Output = Line.substr(FirstQuote + 1, SecondQuote - FirstQuote - 1);
	return true;
}

[[nodiscard]] std::string MakeCycleKey(const std::filesystem::path &Path)
{
	const std::filesystem::path Absolute = std::filesystem::absolute(Path).lexically_normal();
	std::string Key = Absolute.generic_string();
	for (char &Character : Key)
		Character = static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
	return Key;
}
} // namespace

ShaderPreprocessResult ShaderPreprocessor::Preprocess(const ShaderSourceAsset &Source, const ShaderPermutationKey &Permutation,
													  const std::filesystem::path &TrustedRoot) const
{
	ShaderPreprocessResult Result;
	std::unordered_set<std::string> Active;
	const std::filesystem::path IncludeRoot = std::filesystem::absolute(TrustedRoot).lexically_normal();
	try
	{
		core::io::SecurePath::VerifyContained(IncludeRoot, Source.GetSourcePath(), "Root shader source");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw ShaderPreprocessException(Source.GetStage(), Source.GetSourcePath(), Permutation,
										"Root shader is outside the trusted source root: " + string(Exception.what()));
	}
	uint64 TotalSourceBytes = Source.GetSource().size();
	if (TotalSourceBytes > MaximumShaderSourceBytes)
		throw ShaderPreprocessException(Source.GetStage(), Source.GetSourcePath(), Permutation,
										"Root shader source exceeds the configured limit");
	const auto Process = [&](auto &&Self, const std::filesystem::path &Path, const std::string &Text, uint32 SourceIndex,
							 uint32 Depth) -> void
	{
		if (Depth > MaximumShaderIncludeDepth)
			throw ShaderPreprocessException(Source.GetStage(), Path, Permutation, "Include depth exceeds the configured limit");
		const std::string Canonical = MakeCycleKey(Path);
		if (!Active.insert(Canonical).second)
			throw ShaderPreprocessException(Source.GetStage(), Path, Permutation, "Include cycle detected");
		std::istringstream Lines(Text);
		std::string Line;
		uint32 LineNumber = 1;
		while (std::getline(Lines, Line))
		{
			std::string Include;
			if (IncludePath(Line, Include))
			{
				const std::filesystem::path Child = (Path.parent_path() / Include).lexically_normal();
				if (Result.Dependencies.size() >= MaximumShaderIncludeFiles)
					throw ShaderPreprocessException(Source.GetStage(), Child, Permutation, "Include count exceeds the configured limit");
				const std::string ChildSource = ReadSource(IncludeRoot, Child, Source.GetStage(), Permutation);
				if (ChildSource.size() > MaximumShaderSourceBytes - std::min<uint64>(MaximumShaderSourceBytes, TotalSourceBytes))
					throw ShaderPreprocessException(Source.GetStage(), Child, Permutation,
													"Combined shader source exceeds the configured limit");
				TotalSourceBytes += ChildSource.size();
				const uint32 ChildIndex = static_cast<uint32>(Result.Dependencies.size() + 1);
				Result.Dependencies.push_back(Child);
				Result.Source += "#line 1 " + std::to_string(ChildIndex) + "\n";
				Self(Self, Child, ChildSource, ChildIndex, Depth + 1U);
				Result.Source += "#line " + std::to_string(LineNumber + 1) + " " + std::to_string(SourceIndex) + "\n";
			}
			else
				Result.Source += Line + "\n";
			++LineNumber;
		}
		Active.erase(Canonical);
	};

	const usize VersionEnd = Source.GetSource().find('\n');
	if (VersionEnd == std::string::npos || Source.GetSource().rfind("#version", 0) != 0)
		throw ShaderPreprocessException(Source.GetStage(), Source.GetSourcePath(), Permutation, "Root shader must start with #version");
	std::string Root = Source.GetSource();
	const std::string DefineBlock = Permutation.GetDefineBlock();
	Root.insert(VersionEnd + 1, DefineBlock);
	if (!DefineBlock.empty())
		Root.insert(VersionEnd + 1 + DefineBlock.size(), "#line 2 0\n");
	Process(Process, Source.GetSourcePath(), Root, 0, 0);
	return Result;
}
} // namespace pipeline::shader
