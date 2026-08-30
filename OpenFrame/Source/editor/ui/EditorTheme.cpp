#include "EditorTheme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace editor::ui
{
namespace
{
[[nodiscard]] const EditorThemeTokens &LogicalTokens() noexcept
{
	static const EditorThemeTokens Value;
	return Value;
}

void ScaleMetrics(EditorThemeTokens &Tokens, const float32 Scale) noexcept
{
	Tokens.WindowPadding = ImVec2(Tokens.WindowPadding.x * Scale, Tokens.WindowPadding.y * Scale);
	Tokens.FramePadding = ImVec2(Tokens.FramePadding.x * Scale, Tokens.FramePadding.y * Scale);
	Tokens.ItemSpacing = ImVec2(Tokens.ItemSpacing.x * Scale, Tokens.ItemSpacing.y * Scale);
	Tokens.ItemInnerSpacing = ImVec2(Tokens.ItemInnerSpacing.x * Scale, Tokens.ItemInnerSpacing.y * Scale);
	Tokens.CellPadding = ImVec2(Tokens.CellPadding.x * Scale, Tokens.CellPadding.y * Scale);
#define OPENFRAME_SCALE_TOKEN(Name) Tokens.Name *= Scale
	OPENFRAME_SCALE_TOKEN(WindowRounding);
	OPENFRAME_SCALE_TOKEN(ChildRounding);
	OPENFRAME_SCALE_TOKEN(FrameRounding);
	OPENFRAME_SCALE_TOKEN(PopupRounding);
	OPENFRAME_SCALE_TOKEN(ScrollbarRounding);
	OPENFRAME_SCALE_TOKEN(GrabRounding);
	OPENFRAME_SCALE_TOKEN(TabRounding);
	OPENFRAME_SCALE_TOKEN(BorderWidth);
	OPENFRAME_SCALE_TOKEN(FocusWidth);
	OPENFRAME_SCALE_TOKEN(SignatureRailWidth);
	OPENFRAME_SCALE_TOKEN(SignatureHighlightWidth);
	OPENFRAME_SCALE_TOKEN(TreeRowRounding);
	OPENFRAME_SCALE_TOKEN(ButtonRounding);
	OPENFRAME_SCALE_TOKEN(CardRounding);
	OPENFRAME_SCALE_TOKEN(PanelHeaderHeight);
	OPENFRAME_SCALE_TOKEN(ControlHeight);
	OPENFRAME_SCALE_TOKEN(CompactControlHeight);
	OPENFRAME_SCALE_TOKEN(ToolbarHeight);
	OPENFRAME_SCALE_TOKEN(RibbonHeight);
	OPENFRAME_SCALE_TOKEN(RibbonButtonHeight);
	OPENFRAME_SCALE_TOKEN(WorkspaceStripHeight);
	OPENFRAME_SCALE_TOKEN(ViewportControlHeight);
	OPENFRAME_SCALE_TOKEN(DialogActionHeight);
	OPENFRAME_SCALE_TOKEN(HomeActionHeight);
	OPENFRAME_SCALE_TOKEN(HomeContentHorizontalInset);
	OPENFRAME_SCALE_TOKEN(HomeContentBottomInset);
	OPENFRAME_SCALE_TOKEN(HomeContentMaximumWidth);
	OPENFRAME_SCALE_TOKEN(HomeHeroHeight);
	OPENFRAME_SCALE_TOKEN(HomeHeroSummaryWidth);
	OPENFRAME_SCALE_TOKEN(HomeHeroTextMinimumWidth);
	OPENFRAME_SCALE_TOKEN(HomeSectionGap);
	OPENFRAME_SCALE_TOKEN(HomeProjectCardWidth);
	OPENFRAME_SCALE_TOKEN(HomeProjectCardHeight);
	OPENFRAME_SCALE_TOKEN(HomeProjectThumbnailHeight);
	OPENFRAME_SCALE_TOKEN(HomeEmptyStateHeight);
	OPENFRAME_SCALE_TOKEN(TreeTypeRowHeight);
	OPENFRAME_SCALE_TOKEN(TreeRowHeight);
	OPENFRAME_SCALE_TOKEN(OpticalIconMargin);
	OPENFRAME_SCALE_TOKEN(IconCellWidth);
	OPENFRAME_SCALE_TOKEN(IconLabelGap);
	OPENFRAME_SCALE_TOKEN(HierarchyIndent);
#undef OPENFRAME_SCALE_TOKEN
}
} // namespace

const EditorThemeTokens &EditorTheme::GetLogicalTokens() noexcept
{
	return LogicalTokens();
}

float32 EditorTheme::GetScale() noexcept
{
	const float32 RawScale = ImGui::GetCurrentContext() == nullptr ? 1.0F : ImGui::GetStyle().FontScaleDpi;
	return std::isfinite(RawScale) ? std::clamp(RawScale, 0.5F, 4.0F) : 1.0F;
}

float32 EditorTheme::ScaleLogical(const float32 Value) noexcept
{
	return Value * GetScale();
}

ImVec2 EditorTheme::ScaleLogical(const ImVec2 Value) noexcept
{
	const float32 Scale = GetScale();
	return ImVec2(Value.x * Scale, Value.y * Scale);
}

const EditorThemeTokens &EditorTheme::GetTokens() noexcept
{
	const float32 Scale = GetScale();
	if (std::abs(Scale - 1.0F) <= 0.0001F)
		return LogicalTokens();
	thread_local EditorThemeTokens Scaled;
	thread_local float32 CachedScale = 0.0F;
	if (std::abs(Scale - CachedScale) > 0.0001F)
	{
		Scaled = LogicalTokens();
		ScaleMetrics(Scaled, Scale);
		CachedScale = Scale;
	}
	return Scaled;
}

void EditorTheme::ApplyDark()
{
	const EditorThemeTokens &Tokens = GetLogicalTokens();
	ImGuiStyle &Style = ImGui::GetStyle();
	Style = ImGuiStyle{};
	Style.Alpha = 1.0F;
	Style.DisabledAlpha = 0.55F;
	Style.WindowPadding = Tokens.WindowPadding;
	Style.FramePadding = Tokens.FramePadding;
	Style.CellPadding = Tokens.CellPadding;
	Style.ItemSpacing = Tokens.ItemSpacing;
	Style.ItemInnerSpacing = Tokens.ItemInnerSpacing;
	Style.TouchExtraPadding = ImVec2(0.0F, 0.0F);
	Style.IndentSpacing = Tokens.HierarchyIndent;
	Style.ScrollbarSize = 12.0F;
	Style.GrabMinSize = 12.0F;
	Style.WindowRounding = Tokens.WindowRounding;
	Style.ChildRounding = Tokens.ChildRounding;
	Style.FrameRounding = Tokens.FrameRounding;
	Style.PopupRounding = Tokens.PopupRounding;
	Style.ScrollbarRounding = Tokens.ScrollbarRounding;
	Style.GrabRounding = Tokens.GrabRounding;
	Style.TabRounding = Tokens.TabRounding;
	Style.WindowBorderSize = Tokens.BorderWidth;
	Style.ChildBorderSize = Tokens.BorderWidth;
	Style.PopupBorderSize = Tokens.BorderWidth;
	Style.FrameBorderSize = Tokens.BorderWidth;
	Style.TabBorderSize = Tokens.BorderWidth;
	Style.DockingSeparatorSize = 2.0f;
	Style.Colors[ImGuiCol_Text] = Tokens.TextPrimary;
	Style.Colors[ImGuiCol_TextDisabled] = Tokens.TextMuted;
	Style.Colors[ImGuiCol_WindowBg] = Tokens.Canvas;
	Style.Colors[ImGuiCol_ChildBg] = Tokens.Panel;
	Style.Colors[ImGuiCol_PopupBg] = Tokens.PanelRaised;
	Style.Colors[ImGuiCol_Border] = Tokens.BorderSubtle;
	Style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
	Style.Colors[ImGuiCol_FrameBg] = Tokens.Surface;
	Style.Colors[ImGuiCol_FrameBgHovered] = Tokens.SurfaceHover;
	Style.Colors[ImGuiCol_FrameBgActive] = Tokens.SurfacePressed;
	Style.Colors[ImGuiCol_TitleBg] = Tokens.PanelRaised;
	Style.Colors[ImGuiCol_TitleBgActive] = Tokens.PanelRaised;
	Style.Colors[ImGuiCol_TitleBgCollapsed] = Tokens.Panel;
	Style.Colors[ImGuiCol_MenuBarBg] = Tokens.PanelRaised;
	Style.Colors[ImGuiCol_ScrollbarBg] = Tokens.Canvas;
	Style.Colors[ImGuiCol_ScrollbarGrab] = Tokens.ScrollbarThumb;
	Style.Colors[ImGuiCol_ScrollbarGrabHovered] = Tokens.ScrollbarThumbHover;
	Style.Colors[ImGuiCol_ScrollbarGrabActive] = Tokens.ScrollbarThumbPressed;
	Style.Colors[ImGuiCol_CheckMark] = Tokens.Accent;
	Style.Colors[ImGuiCol_SliderGrab] = Tokens.Accent;
	Style.Colors[ImGuiCol_SliderGrabActive] = Tokens.AccentHover;
	Style.Colors[ImGuiCol_Button] = Tokens.Surface;
	Style.Colors[ImGuiCol_ButtonHovered] = Tokens.SurfaceHover;
	Style.Colors[ImGuiCol_ButtonActive] = Tokens.SurfacePressed;
	Style.Colors[ImGuiCol_Header] = Tokens.SurfaceSelected;
	Style.Colors[ImGuiCol_HeaderHovered] = Tokens.SurfaceSelectedHover;
	Style.Colors[ImGuiCol_HeaderActive] = Tokens.AccentPressed;
	Style.Colors[ImGuiCol_Separator] = Tokens.BorderSubtle;
	Style.Colors[ImGuiCol_SeparatorHovered] = Tokens.BorderStrong;
	Style.Colors[ImGuiCol_SeparatorActive] = Tokens.Accent;
	Style.Colors[ImGuiCol_ResizeGrip] = Tokens.BorderSubtle;
	Style.Colors[ImGuiCol_ResizeGripHovered] = Tokens.AccentHover;
	Style.Colors[ImGuiCol_ResizeGripActive] = Tokens.Accent;
	Style.Colors[ImGuiCol_Tab] = Tokens.Surface;
	Style.Colors[ImGuiCol_TabHovered] = Tokens.SurfaceHover;
	Style.Colors[ImGuiCol_TabSelected] = Tokens.SurfaceSelected;
	Style.Colors[ImGuiCol_TabSelectedOverline] = Tokens.Accent;
	Style.Colors[ImGuiCol_TabDimmed] = Tokens.Panel;
	Style.Colors[ImGuiCol_TabDimmedSelected] = Tokens.SurfaceSelected;
	Style.Colors[ImGuiCol_TabDimmedSelectedOverline] = Tokens.AccentPressed;
	Style.Colors[ImGuiCol_DockingPreview] = ImVec4(Tokens.Accent.x, Tokens.Accent.y, Tokens.Accent.z, 0.45F);
	Style.Colors[ImGuiCol_DockingEmptyBg] = Tokens.Canvas;
	Style.Colors[ImGuiCol_PlotLines] = Tokens.Info;
	Style.Colors[ImGuiCol_PlotLinesHovered] = Tokens.AccentHover;
	Style.Colors[ImGuiCol_PlotHistogram] = Tokens.Accent;
	Style.Colors[ImGuiCol_PlotHistogramHovered] = Tokens.AccentHover;
	Style.Colors[ImGuiCol_TableHeaderBg] = Tokens.PanelRaised;
	Style.Colors[ImGuiCol_TableBorderStrong] = Tokens.BorderStrong;
	Style.Colors[ImGuiCol_TableBorderLight] = Tokens.BorderSubtle;
	Style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
	Style.Colors[ImGuiCol_TableRowBgAlt] = Style.Colors[ImGuiCol_TableRowBg];
	Style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(Tokens.Accent.x, Tokens.Accent.y, Tokens.Accent.z, 0.35F);
	Style.Colors[ImGuiCol_DragDropTarget] = Tokens.Accent;
	Style.Colors[ImGuiCol_NavHighlight] = Tokens.FocusRing;
	Style.Colors[ImGuiCol_NavWindowingHighlight] = Tokens.FocusRing;
	Style.Colors[ImGuiCol_NavWindowingDimBg] = Tokens.Overlay;
	Style.Colors[ImGuiCol_ModalWindowDimBg] = Tokens.Overlay;
}
} // namespace editor::ui
