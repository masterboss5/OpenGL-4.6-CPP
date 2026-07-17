#include "ShaderPreprocessor.h"

#include <fstream>
#include <sstream>
#include <unordered_set>

#include "ShaderException.h"

namespace pipeline::shader
{
	namespace
	{
		std::string readSource(const std::filesystem::path& path, ShaderStage stage, const ShaderPermutationKey& key)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file.is_open()) throw ShaderPreprocessException(stage, path, key, "Unable to open included source");
			std::ostringstream text; text << file.rdbuf();
			if (file.bad()) throw ShaderPreprocessException(stage, path, key, "I/O failure while reading included source");
			return text.str();
		}
		bool includePath(const std::string& line, std::string& output)
		{
			const size_t include = line.find("#include");
			if (include == std::string::npos) return false;
			const size_t firstQuote = line.find('"', include);
			const size_t secondQuote = firstQuote == std::string::npos ? std::string::npos : line.find('"', firstQuote + 1);
			if (firstQuote == std::string::npos || secondQuote == std::string::npos) return false;
			output = line.substr(firstQuote + 1, secondQuote - firstQuote - 1); return true;
		}
	}

	ShaderPreprocessResult ShaderPreprocessor::preprocess(const ShaderSourceAsset& source, const ShaderPermutationKey& permutation) const
	{
		ShaderPreprocessResult result;
		std::unordered_set<std::string> active;
		const auto process = [&](auto&& self, const std::filesystem::path& path, const std::string& text, uint32 sourceIndex) -> void
		{
			const std::string canonical = std::filesystem::absolute(path).lexically_normal().generic_string();
			if (!active.insert(canonical).second) throw ShaderPreprocessException(source.getStage(), path, permutation, "Include cycle detected");
			std::istringstream lines(text); std::string line; uint32 lineNumber = 1;
			while (std::getline(lines, line))
			{
				std::string include;
				if (includePath(line, include))
				{
					const std::filesystem::path child = (path.parent_path() / include).lexically_normal();
					const uint32 childIndex = static_cast<uint32>(result.dependencies.size() + 1);
					result.dependencies.push_back(child);
					result.source += "#line 1 " + std::to_string(childIndex) + "\n";
					self(self, child, readSource(child, source.getStage(), permutation), childIndex);
					result.source += "#line " + std::to_string(lineNumber + 1) + " " + std::to_string(sourceIndex) + "\n";
				}
				else result.source += line + "\n";
				++lineNumber;
			}
			active.erase(canonical);
		};

		const size_t versionEnd = source.getSource().find('\n');
		if (versionEnd == std::string::npos || source.getSource().rfind("#version", 0) != 0) throw ShaderPreprocessException(source.getStage(), source.getSourcePath(), permutation, "Root shader must start with #version");
		std::string root = source.getSource();
		root.insert(versionEnd + 1, permutation.getDefineBlock());
		process(process, source.getSourcePath(), root, 0);
		return result;
	}
}
