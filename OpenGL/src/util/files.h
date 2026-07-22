#pragma once
#include "src/types.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace file
{
struct FileSource final
{
	std::string name;
	std::string extension;
	std::filesystem::path path;
	std::string source;
	uint64 bytesSize;

	[[nodiscard]] explicit operator bool() const
	{
		return !source.empty();
	}
};

inline bool fileExists(const std::filesystem::path &path)
{
	return std::filesystem::exists(path);
}

inline FileSource readFile(const std::filesystem::path &path)
{
	if (!fileExists(path))
	{
		// TODO add failure logic later
		return {};
	}

	std::ifstream file(path);
	if (!file.is_open())
	{
		// TODO add failure logic later
		return {};
	}

	std::stringstream buffer;
	buffer << file.rdbuf();

	FileSource fileSource{};
	fileSource.name = path.stem().string();
	fileSource.extension = path.extension().string();
	fileSource.path = path;
	fileSource.source = buffer.str();
	fileSource.bytesSize = std::filesystem::file_size(path);

	return fileSource;
}
} // namespace file
