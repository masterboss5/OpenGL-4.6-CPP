#pragma once

#include "EditorStyle.h"

#include "Source/types.h"

#include <imgui.h>

namespace editor::ui
{
class EditorIconRegistry;

struct EditorButtonOptions final
{
	ImVec2 Size;
	EditorControlRole Role = EditorControlRole::Neutral;
	bool Selected = false;
	bool Enabled = true;
	bool AllowLabelClipping = false;
	string_view Tooltip;
};

struct EditorProjectCardOptions final
{
	ImTextureRef Thumbnail;
	ImVec2 Size;
	string_view Name;
	string_view LastEdited;
	string_view DescriptorPath;
	bool Available = true;
};

struct EditorProjectCardResult final
{
	bool OpenRequested = false;
	bool RemoveRequested = false;
};

class EditorWidgets final
{
  public:
	[[nodiscard]] static bool Button(const char *Label, ImVec2 Size = ImVec2(0.0F, 0.0F),
									 EditorControlRole Role = EditorControlRole::Neutral, bool Selected = false);
	[[nodiscard]] static bool Button(const char *Label, const EditorButtonOptions &Options);
	[[nodiscard]] static bool SmallButton(const char *Label, EditorControlRole Role = EditorControlRole::Quiet, bool Selected = false);
	[[nodiscard]] static bool Selectable(const char *Label, bool Selected = false, ImGuiSelectableFlags Flags = 0,
										 ImVec2 Size = ImVec2(0.0F, 0.0F));
	[[nodiscard]] static bool Checkbox(const char *Label, bool *Value);
	[[nodiscard]] static bool CollapsingHeader(const char *Label, ImGuiTreeNodeFlags Flags = 0);
	[[nodiscard]] static bool CollapsingHeader(const char *Label, bool *Visible, ImGuiTreeNodeFlags Flags = 0);
	[[nodiscard]] static bool InputText(const char *Label, string &Value);
	[[nodiscard]] static bool SearchField(const char *Label, string &Value, const EditorIconRegistry &Icons, string_view Hint = "Search");
	[[nodiscard]] static EditorProjectCardResult ProjectCard(const char *ID, const EditorProjectCardOptions &Options);
};
} // namespace editor::ui
