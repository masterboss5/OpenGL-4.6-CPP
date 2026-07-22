#include "ShaderPreprocessor.h"

#include "ShaderException.h"

#include <fstream>
#include <sstream>
#include <unordered_set>

namespace pipeline::shader
{
namespace
{
std::string ReadSource(const std::filesystem::path &Path, ShaderStage Stage, const ShaderPermutationKey &Key)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File.is_open())
		throw ShaderPreprocessException(Stage, Path, Key, "Unable to open included source");
	std::ostringstream Text;
	Text << File.rdbuf();
	if (File.bad())
		throw ShaderPreprocessException(Stage, Path, Key, "I/O failure while reading included source");
	return Text.str();
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
} // namespace

ShaderPreprocessResult ShaderPreprocessor::Preprocess(const ShaderSourceAsset &Source, const ShaderPermutationKey &Permutation) const
{
	ShaderPreprocessResult Result;
	std::unordered_set<std::string> Active;
	const auto Process = [&](auto &&Self, const std::filesystem::path &Path, const std::string &Text, uint32 SourceIndex) -> void
	{
		const std::string Canonical = std::filesystem::absolute(Path).lexically_normal().generic_string();
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
				const uint32 ChildIndex = static_cast<uint32>(Result.Dependencies.size() + 1);
				Result.Dependencies.push_back(Child);
				Result.Source += "#line 1 " + std::to_string(ChildIndex) + "\n";
				Self(Self, Child, ReadSource(Child, Source.GetStage(), Permutation), ChildIndex);
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
	Root.insert(VersionEnd + 1, Permutation.GetDefineBlock());
	Process(Process, Source.GetSourcePath(), Root, 0);
	return Result;
}
} // namespace pipeline::shader
