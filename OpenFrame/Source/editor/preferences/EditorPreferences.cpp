#include "EditorPreferences.h"

#include "Source/core/io/SecurePath.h"
#include "Source/util/UUID.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <vector>

namespace editor::preferences
{
namespace
{
using Json = nlohmann::json;

[[nodiscard]] std::span<const uint8> BytesOf(const string &Text) noexcept
{
	return {reinterpret_cast<const uint8 *>(Text.data()), Text.size()};
}
} // namespace

void EditorPreferences::Validate() const
{
	if (this->FormatVersion != CurrentFormatVersion)
		throw EditorPreferencesException("Editor preferences use an unsupported format version");
	if (this->AutosaveIntervalSeconds < 10 || this->AutosaveIntervalSeconds > 3'600)
		throw EditorPreferencesException("Autosave interval must be between 10 and 3600 seconds");
	if (this->AutosaveQuietPeriodSeconds > 60)
		throw EditorPreferencesException("Autosave quiet period cannot exceed 60 seconds");
	if (this->CommandHistoryCapacity < 32 || this->CommandHistoryCapacity > 65'536)
		throw EditorPreferencesException("Command history capacity must be between 32 and 65536");
	if (this->CameraMoveSpeed < 0.01f || this->CameraMoveSpeed > 10'000.0f)
		throw EditorPreferencesException("Camera movement speed is outside the supported range");
	if (this->CameraLookSensitivity < 0.001f || this->CameraLookSensitivity > 10.0f)
		throw EditorPreferencesException("Camera look sensitivity is outside the supported range");
	if (!std::isfinite(this->TranslationSnap) || this->TranslationSnap <= 0.0f || !std::isfinite(this->RotationSnapDegrees) ||
		this->RotationSnapDegrees <= 0.0f || !std::isfinite(this->ScaleSnap) || this->ScaleSnap <= 0.0f)
	{
		throw EditorPreferencesException("Transform snap increments must be finite and positive");
	}
}

EditorPreferences EditorPreferencesStore::Load(const std::filesystem::path &Path)
{
	if (!std::filesystem::is_regular_file(Path))
		return {};
	try
	{
		constexpr uint64 MaximumPreferencesBytes = 1024U * 1024U;
		const std::filesystem::path DirectoryRoot = std::filesystem::absolute(Path.parent_path()).lexically_normal();
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(DirectoryRoot, Path.filename(), MaximumPreferencesBytes, "editor preferences");
		const Json Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
		EditorPreferences Result{.FormatVersion = Root.at("FormatVersion").get<uint32>(),
								 .AutosaveEnabled = Root.value("AutosaveEnabled", true),
								 .AutosaveIntervalSeconds = Root.value("AutosaveIntervalSeconds", 60U),
								 .AutosaveQuietPeriodSeconds = Root.value("AutosaveQuietPeriodSeconds", 3U),
								 .CommandHistoryCapacity = Root.value("CommandHistoryCapacity", 1'024U),
								 .CameraMoveSpeed = Root.value("CameraMoveSpeed", 8.0f),
								 .CameraLookSensitivity = Root.value("CameraLookSensitivity", 0.12f),
								 .ShowGridByDefault = Root.value("ShowGridByDefault", true),
								 .TransformSnappingEnabled = Root.value("TransformSnappingEnabled", false),
								 .TranslationSnap = Root.value("TranslationSnap", 1.0f),
								 .RotationSnapDegrees = Root.value("RotationSnapDegrees", 15.0f),
								 .ScaleSnap = Root.value("ScaleSnap", 0.1f)};
		Result.Validate();
		return Result;
	}
	catch (const std::exception &Exception)
	{
		throw EditorPreferencesException("Could not load editor preferences '" + Path.string() + "': " + Exception.what());
	}
}

void EditorPreferencesStore::Save(const EditorPreferences &Preferences, const std::filesystem::path &Path)
{
	Preferences.Validate();
	const Json Root{{"FormatVersion", EditorPreferences::CurrentFormatVersion},
					{"AutosaveEnabled", Preferences.AutosaveEnabled},
					{"AutosaveIntervalSeconds", Preferences.AutosaveIntervalSeconds},
					{"AutosaveQuietPeriodSeconds", Preferences.AutosaveQuietPeriodSeconds},
					{"CommandHistoryCapacity", Preferences.CommandHistoryCapacity},
					{"CameraMoveSpeed", Preferences.CameraMoveSpeed},
					{"CameraLookSensitivity", Preferences.CameraLookSensitivity},
					{"ShowGridByDefault", Preferences.ShowGridByDefault},
					{"TransformSnappingEnabled", Preferences.TransformSnappingEnabled},
					{"TranslationSnap", Preferences.TranslationSnap},
					{"RotationSnapDegrees", Preferences.RotationSnapDegrees},
					{"ScaleSnap", Preferences.ScaleSnap}};
	const std::filesystem::path DirectoryRoot = std::filesystem::absolute(Path.parent_path()).lexically_normal();
	const std::filesystem::path Destination = Path.filename();
	const std::filesystem::path Temporary = Destination.string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	const string Serialized = Root.dump(2) + '\n';
	core::io::SecurePath::CreateTrustedRoot(DirectoryRoot, "editor preferences root");
	core::io::SecurePath::WriteFileWithin(DirectoryRoot, Temporary, BytesOf(Serialized), false, true, "editor preferences temporary file");
	try
	{
		core::io::SecurePath::ReplaceWithin(DirectoryRoot, Temporary, Destination, "editor preferences publication");
	}
	catch (const std::exception &Exception)
	{
		try
		{
			core::io::SecurePath::RemoveWithin(DirectoryRoot, Temporary, false, "editor preferences temporary cleanup");
		}
		catch (...)
		{
		}
		throw EditorPreferencesException("Could not securely publish editor preferences: " + string(Exception.what()));
	}
}
} // namespace editor::preferences
