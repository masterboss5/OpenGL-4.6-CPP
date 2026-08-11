#pragma once

#include "src/types.h"

#include <filesystem>

namespace editor::validation
{
void RunDeterministicEditorCoreChecks();
void RunDeterministicGameModuleChecks(const std::filesystem::path &ValidModule, const std::filesystem::path &InvalidModule);
void RunDeterministicProjectBuildChecks(const std::filesystem::path &MSBuild, const std::filesystem::path &Solution,
										const std::filesystem::path &BuiltModule, string Configuration);
} // namespace editor::validation
