#pragma once

#include "EditorTheme.h"

#include "Source/types.h"

#include <imgui.h>

namespace editor::ui
{
enum class EditorControlRole : uint8
{
	Neutral,
	Primary,
	Quiet,
	Toolbar,
	Danger,
	Row
};

enum class EditorInteractionState : uint8
{
	Idle,
	Hovered,
	Pressed,
	Selected,
	SelectedHovered,
	SelectedPressed,
	Disabled
};

struct EditorControlVisual final
{
	ImVec4 Fill;
	ImVec4 Border;
	ImVec4 Text;
	ImVec4 Accent;
	bool DrawAccent = false;
};

class EditorStyleSystem final
{
  public:
	static void ApplyDefaultDark(float32 LayoutScale = 1.0F);

	[[nodiscard]] static EditorInteractionState ResolveState(bool Selected, bool Hovered, bool Pressed, bool Enabled) noexcept;
	[[nodiscard]] static EditorControlVisual Resolve(EditorControlRole Role, EditorInteractionState State) noexcept;
};
} // namespace editor::ui
