#pragma once

#include "src/editor/project/Project.h"

#include <filesystem>
#include <stdexcept>

namespace editor::serialization
{
class ProjectDescriptorSerializationException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ProjectDescriptorSerializer final
{
  public:
	static constexpr uint32 CurrentFormatVersion = 1;

	[[nodiscard]] static project::ProjectDescriptor Load(const std::filesystem::path &Path);
	static void Save(const project::ProjectDescriptor &Descriptor, const std::filesystem::path &Path = {});
};
} // namespace editor::serialization
