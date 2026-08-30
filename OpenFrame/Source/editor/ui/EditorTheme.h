#pragma once

#include "Source/types.h"

#include <imgui.h>

namespace editor::ui
{
struct EditorThemeTokens final
{
	ImVec4 Canvas{0.031F, 0.043F, 0.047F, 1.0F};
	ImVec4 Panel{0.055F, 0.078F, 0.086F, 1.0F};
	ImVec4 PanelRaised{0.078F, 0.114F, 0.125F, 1.0F};
	ImVec4 Surface{0.098F, 0.145F, 0.161F, 1.0F};
	ImVec4 SurfaceHover{0.133F, 0.204F, 0.227F, 1.0F};
	ImVec4 SurfacePressed{0.169F, 0.263F, 0.290F, 1.0F};
	ImVec4 SurfaceSelected{0.071F, 0.231F, 0.235F, 1.0F};
	ImVec4 SurfaceSelectedHover{0.094F, 0.322F, 0.329F, 1.0F};
	ImVec4 SurfaceInset{0.039F, 0.063F, 0.071F, 1.0F};
	ImVec4 SurfaceElevated{0.125F, 0.188F, 0.204F, 1.0F};
	ImVec4 ToolbarSurface{0.067F, 0.098F, 0.106F, 1.0F};
	ImVec4 BorderSubtle{0.133F, 0.188F, 0.204F, 1.0F};
	ImVec4 BorderStrong{0.227F, 0.318F, 0.337F, 1.0F};
	ImVec4 HighlightLine{0.941F, 0.643F, 0.365F, 0.220F};
	ImVec4 TextPrimary{0.914F, 0.949F, 0.937F, 1.0F};
	ImVec4 TextSecondary{0.671F, 0.753F, 0.733F, 1.0F};
	ImVec4 TextMuted{0.443F, 0.537F, 0.522F, 1.0F};
	ImVec4 TextDisabled{0.275F, 0.353F, 0.341F, 1.0F};
	ImVec4 Accent{0.180F, 0.788F, 0.718F, 1.0F};
	ImVec4 AccentHover{0.314F, 0.867F, 0.792F, 1.0F};
	ImVec4 AccentPressed{0.102F, 0.584F, 0.529F, 1.0F};
	ImVec4 FocusRing{0.941F, 0.643F, 0.365F, 1.0F};
	ImVec4 Success{0.302F, 0.839F, 0.631F, 1.0F};
	ImVec4 Warning{0.941F, 0.702F, 0.369F, 1.0F};
	ImVec4 Danger{0.925F, 0.353F, 0.361F, 1.0F};
	ImVec4 Info{0.337F, 0.749F, 0.820F, 1.0F};
	ImVec4 ScrollbarThumb{0.180F, 0.263F, 0.278F, 1.0F};
	ImVec4 ScrollbarThumbHover{0.247F, 0.380F, 0.396F, 1.0F};
	ImVec4 ScrollbarThumbPressed{0.314F, 0.506F, 0.514F, 1.0F};
	ImVec4 Overlay{0.012F, 0.024F, 0.027F, 0.760F};
	ImVec4 Shadow{0.0F, 0.0F, 0.0F, 0.400F};

	ImVec2 WindowPadding{12.0F, 8.0F};
	ImVec2 FramePadding{10.0F, 6.0F};
	ImVec2 ItemSpacing{8.0F, 8.0F};
	ImVec2 ItemInnerSpacing{8.0F, 6.0F};
	ImVec2 CellPadding{8.0F, 5.0F};
	float32 WindowRounding = 8.0F;
	float32 ChildRounding = 8.0F;
	float32 FrameRounding = 8.0F;
	float32 PopupRounding = 10.0F;
	float32 ScrollbarRounding = 4.0F;
	float32 GrabRounding = 4.0F;
	float32 TabRounding = 8.0F;
	float32 BorderWidth = 1.0F;
	float32 FocusWidth = 2.0F;
	float32 SignatureRailWidth = 3.0F;
	float32 SignatureHighlightWidth = 1.0F;
	float32 TreeRowRounding = 6.0F;
	float32 ButtonRounding = 8.0F;
	float32 CardRounding = 10.0F;
	float32 PanelHeaderHeight = 36.0F;
	float32 ControlHeight = 32.0F;
	float32 CompactControlHeight = 28.0F;
	float32 ToolbarHeight = 40.0F;
	float32 RibbonHeight = 88.0F;
	float32 RibbonButtonHeight = 54.0F;
	float32 WorkspaceStripHeight = 40.0F;
	float32 ViewportControlHeight = 34.0F;
	float32 DialogActionHeight = 36.0F;
	float32 HomeActionHeight = 42.0F;
	float32 HomeContentHorizontalInset = 24.0F;
	float32 HomeContentBottomInset = 24.0F;
	float32 HomeContentMaximumWidth = 1'440.0F;
	float32 HomeHeroHeight = 224.0F;
	float32 HomeHeroSummaryWidth = 284.0F;
	float32 HomeHeroTextMinimumWidth = 360.0F;
	float32 HomeSectionGap = 20.0F;
	float32 HomeProjectCardWidth = 320.0F;
	float32 HomeProjectCardHeight = 246.0F;
	float32 HomeProjectThumbnailHeight = 180.0F;
	float32 HomeEmptyStateHeight = 264.0F;
	float32 TreeTypeRowHeight = 38.0F;
	float32 TreeRowHeight = 20.0F;
	float32 OpticalIconMargin = 2.0F;
	float32 IconCellWidth = 24.0F;
	float32 IconLabelGap = 8.0F;
	float32 HierarchyIndent = 18.0F;
};

class EditorTheme final
{
  public:
	static void ApplyDark();

	[[nodiscard]] static float32 GetScale() noexcept;
	[[nodiscard]] static float32 ScaleLogical(float32 Value) noexcept;
	[[nodiscard]] static ImVec2 ScaleLogical(ImVec2 Value) noexcept;
	[[nodiscard]] static const EditorThemeTokens &GetTokens() noexcept;
	[[nodiscard]] static const EditorThemeTokens &GetLogicalTokens() noexcept;
};
} // namespace editor::ui
