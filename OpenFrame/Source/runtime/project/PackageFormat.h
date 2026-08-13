#pragma once

#include "Source/types.h"

namespace runtime::project
{
inline constexpr uint32 ProjectPackageFormatVersion = 2;

enum class PackageFileKind : uint8
{
	Executable,
	DynamicLibrary,
	GameModule,
	EngineContent
};
} // namespace runtime::project
