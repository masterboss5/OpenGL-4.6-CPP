#include "EditorStyle.h"

#include <algorithm>

namespace editor::ui
{
namespace
{
[[nodiscard]] ImVec4 Blend(const ImVec4 Left, const ImVec4 Right, const float32 Amount) noexcept
{
	const float32 Inverse = 1.0F - Amount;
	return ImVec4(Left.x * Inverse + Right.x * Amount, Left.y * Inverse + Right.y * Amount, Left.z * Inverse + Right.z * Amount,
				  Left.w * Inverse + Right.w * Amount);
}
} // namespace

void EditorStyleSystem::ApplyDefaultDark(const float32 LayoutScale)
{
	EditorTheme::ApplyDark();
	ImGuiStyle &Style = ImGui::GetStyle();
	Style.WindowTitleAlign = ImVec2(0.0F, 0.5F);
	Style.WindowMenuButtonPosition = ImGuiDir_Right;
	Style.ButtonTextAlign = ImVec2(0.5F, 0.5F);
	Style.SelectableTextAlign = ImVec2(0.0F, 0.5F);
	Style.SeparatorTextBorderSize = 1.0F;
	Style.SeparatorTextAlign = ImVec2(0.0F, 0.5F);
	Style.SeparatorTextPadding = ImVec2(Style.FramePadding.x, Style.ItemSpacing.y);
	Style.HoverDelayNormal = 0.4F;
	Style.HoverDelayShort = 0.08F;
	Style.AntiAliasedLines = true;
	Style.AntiAliasedLinesUseTex = true;
	Style.AntiAliasedFill = true;
	Style.ScaleAllSizes(std::clamp(LayoutScale, 0.5F, 4.0F));
}

EditorInteractionState EditorStyleSystem::ResolveState(const bool Selected, const bool Hovered, const bool Pressed,
													   const bool Enabled) noexcept
{
	if (!Enabled)
		return EditorInteractionState::Disabled;
	if (Selected)
	{
		if (Pressed)
			return EditorInteractionState::SelectedPressed;
		if (Hovered)
			return EditorInteractionState::SelectedHovered;
		return EditorInteractionState::Selected;
	}
	if (Pressed)
		return EditorInteractionState::Pressed;
	if (Hovered)
		return EditorInteractionState::Hovered;
	return EditorInteractionState::Idle;
}

EditorControlVisual EditorStyleSystem::Resolve(const EditorControlRole Role, const EditorInteractionState State) noexcept
{
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	const bool Disabled = State == EditorInteractionState::Disabled;
	const bool Selected = State == EditorInteractionState::Selected || State == EditorInteractionState::SelectedHovered ||
						  State == EditorInteractionState::SelectedPressed;
	const bool Hovered = State == EditorInteractionState::Hovered || State == EditorInteractionState::SelectedHovered;
	const bool Pressed = State == EditorInteractionState::Pressed || State == EditorInteractionState::SelectedPressed;

	EditorControlVisual Result{
		.Fill = Tokens.Surface, .Border = Tokens.BorderSubtle, .Text = Tokens.TextSecondary, .Accent = Tokens.Accent};
	if (Role == EditorControlRole::Quiet || Role == EditorControlRole::Toolbar)
		Result.Fill = Role == EditorControlRole::Toolbar ? Tokens.ToolbarSurface : ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
	if (Hovered)
	{
		Result.Fill = Tokens.SurfaceHover;
		Result.Text = Tokens.TextPrimary;
		Result.Border = Tokens.BorderStrong;
	}
	if (Pressed)
	{
		Result.Fill = Tokens.SurfacePressed;
		Result.Text = Tokens.TextPrimary;
	}
	if (Selected)
	{
		Result.Fill = Hovered ? Tokens.SurfaceSelectedHover : Tokens.SurfaceSelected;
		if (Pressed)
			Result.Fill = Tokens.AccentPressed;
		Result.Border = Hovered ? Tokens.FocusRing : Tokens.BorderStrong;
		Result.Text = Tokens.TextPrimary;
		Result.DrawAccent = true;
	}
	if (Role == EditorControlRole::Primary)
	{
		Result.Fill = Pressed ? Tokens.AccentPressed : (Hovered ? Tokens.AccentHover : Tokens.Accent);
		Result.Border = Hovered ? Tokens.FocusRing : Tokens.AccentPressed;
		Result.Text = Tokens.TextPrimary;
		Result.Accent = Tokens.FocusRing;
		Result.DrawAccent = true;
	}
	else if (Role == EditorControlRole::Danger)
	{
		Result.Fill = Pressed ? Blend(Tokens.SurfacePressed, Tokens.Danger, 0.34F)
							  : (Hovered ? Blend(Tokens.SurfaceHover, Tokens.Danger, 0.18F) : Tokens.Surface);
		Result.Border = Hovered || Pressed ? Tokens.Danger : Tokens.BorderSubtle;
		Result.Text = Hovered || Pressed ? Tokens.TextPrimary : Tokens.Danger;
		Result.Accent = Tokens.Danger;
		Result.DrawAccent = Hovered || Pressed;
	}
	else if (Role == EditorControlRole::Row && !Selected)
	{
		Result.Fill = Hovered ? Tokens.SurfaceHover : ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
		Result.Border = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
	}
	if (Disabled)
	{
		Result.Fill = Role == EditorControlRole::Quiet ? ImVec4(0.0F, 0.0F, 0.0F, 0.0F) : Tokens.SurfaceInset;
		Result.Border = Tokens.BorderSubtle;
		Result.Text = Tokens.TextDisabled;
		Result.DrawAccent = false;
	}
	return Result;
}
} // namespace editor::ui
