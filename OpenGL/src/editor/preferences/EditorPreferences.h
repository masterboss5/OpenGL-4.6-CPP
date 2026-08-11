#pragma once

#include "src/types.h"

#include <filesystem>
#include <stdexcept>

namespace editor::preferences
{
struct EditorPreferences final
{
	static constexpr uint32 CurrentFormatVersion = 1;

	uint32 FormatVersion = CurrentFormatVersion;
	bool AutosaveEnabled = true;
	uint32 AutosaveIntervalSeconds = 60;
	uint32 AutosaveQuietPeriodSeconds = 3;
	uint32 CommandHistoryCapacity = 1'024;
	float32 CameraMoveSpeed = 8.0f;
	float32 CameraLookSensitivity = 0.12f;
	bool ShowGridByDefault = true;
	bool TransformSnappingEnabled = false;
	float32 TranslationSnap = 1.0f;
	float32 RotationSnapDegrees = 15.0f;
	float32 ScaleSnap = 0.1f;

	void Validate() const;
};

class EditorPreferencesException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class EditorPreferencesStore final
{
  public:
	[[nodiscard]] static EditorPreferences Load(const std::filesystem::path &Path);
	static void Save(const EditorPreferences &Preferences, const std::filesystem::path &Path);
};
} // namespace editor::preferences
