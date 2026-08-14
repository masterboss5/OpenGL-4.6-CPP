#include "EditorIconRegistry.h"

#include <imgui.h>

#include <algorithm>

namespace
{
[[nodiscard]] ImWchar DecodeThreeByteCodepoint(const string_view Text) noexcept
{
	if (Text.size() != 3)
		return 0;
	const uint32 First = static_cast<uint8>(Text[0]);
	const uint32 Second = static_cast<uint8>(Text[1]);
	const uint32 Third = static_cast<uint8>(Text[2]);
	if ((First & 0xF0U) != 0xE0U || (Second & 0xC0U) != 0x80U || (Third & 0xC0U) != 0x80U)
		return 0;
	return static_cast<ImWchar>(((First & 0x0FU) << 12U) | ((Second & 0x3FU) << 6U) | (Third & 0x3FU));
}
} // namespace

namespace editor::ui
{
EditorIconRegistry::EditorIconRegistry()
	: Icons{{"Open", "\xEE\xA3\xA5"},
			{"Save", "\xEE\x9D\x8E"},
			{"Cook", "\xEE\xA2\x92"},
			{"Package", "\xEE\xA2\x93"},
			{"Undo", "\xEE\x9E\xA7"},
			{"Redo", "\xEE\x9E\xA6"},
			{"Import", "\xEE\xA2\xB5"},
			{"Stop", "\xEE\x9C\x9A"},
			{"Cursor", "\xEE\xA2\xB0"},
			{"Move", "\xEE\x9D\xA8"},
			{"Rotate", "\xEE\x9E\xAD"},
			{"Scale", "\xEE\xA2\xAC"},
			{"Axes", "\xEE\xA0\xA7"},
			{"Magnet", "\xEE\xA1\xB5"},
			{"Play", "\xEE\x9D\xA8"},
			{"Pause", "\xEE\x9D\xA9"},
			{"Step", "\xEE\xA2\xAD"},
			{"Panel", "\xEE\x9F\x84"},
			{"Box", "\xEE\xA7\x8A"},
			{"Sphere", "\xEE\xA6\xA3"},
			{"Capsule", "\xEE\xA8\x99"},
			{"Cylinder", "\xEE\xA7\x8A"},
			{"Cone", "\xEE\xA7\x8A"},
			{"Plane", "\xEE\xA7\x8A"},
			{"Settings", "\xEE\x9C\x93"},
			{"Workspace", "\xEE\xA0\xB8"},
			{"Lighting", "\xEE\x9E\xBA"},
			{"GUI", "\xEE\xA3\xB4"},
			{"Audio", "\xEE\x9D\xA7"},
			{"Scripts", "\xEE\xA0\x94"},
			{"Folder", "\xEE\xA3\xA5"},
			{"Model", "\xEE\xA7\x8A"},
			{"Part", "\xEE\xA7\x8A"},
			{"MeshPart", "\xEE\xA6\xA3"},
			{"Camera", "\xEE\xA2\x94"},
			{"Attachment", "\xEE\xA0\xA7"},
			{"DirectionalLight", "\xEE\x9E\xBA"},
			{"PointLight", "\xEE\x9E\xBA"},
			{"SpotLight", "\xEE\x9E\xBA"},
			{"AnimationTrack", "\xEE\xA2\xAD"},
			{"Animator", "\xEE\xA2\xAD"},
			{"Script", "\xEE\xA0\x94"},
			{"ModuleScript", "\xEE\xA0\x94"},
			{"Sound", "\xEE\x9D\xA7"},
			{"AudioEmitter", "\xEE\x9D\xA7"},
			{"AudioListener", "\xEE\x9D\xA7"},
			{"AudioGroup", "\xEE\x9D\xA7"},
			{"ScreenGUI", "\xEE\xA3\xB4"},
			{"Frame", "\xEE\xA3\xB4"},
			{"Text", "\xEE\xA1\xA2"},
			{"Image", "\xEE\xA3\xB4"},
			{"Button", "\xEE\xA3\xB4"},
			{"ScrollView", "\xEE\xA3\xB4"},
			{"Sky", "\xEE\x9E\xBA"},
			{"Atmosphere", "\xEE\x9E\xBA"},
			{"Decal", "\xEE\xA3\xB4"},
			{"ParticleEmitter", "\xEE\x9E\xBA"},
			{"RigidBody", "\xEE\xA7\x8A"},
			{"BoxCollider", "\xEE\xA7\x8A"},
			{"SphereCollider", "\xEE\xA6\xA3"},
			{"CapsuleCollider", "\xEE\xA8\x99"},
			{"MeshCollider", "\xEE\xA7\x8A"},
			{"HingeConstraint", "\xEE\xA0\xA7"},
			{"SpringConstraint", "\xEE\xA0\xA7"}}
{
}

string_view EditorIconRegistry::Find(const string_view Name) const noexcept
{
	const auto Iterator = std::ranges::find_if(this->Icons, [Name](const auto &Entry) { return Entry.first == Name; });
	if (Iterator == this->Icons.end())
		return {};
	const string_view Icon = Iterator->second;
	const ImWchar Codepoint = DecodeThreeByteCodepoint(Icon);
	ImFontBaked *Font = ImGui::GetFontBaked();
	return Codepoint != 0 && Font != nullptr && Font->FindGlyphNoFallback(Codepoint) != nullptr ? Icon : string_view{};
}
} // namespace editor::ui
