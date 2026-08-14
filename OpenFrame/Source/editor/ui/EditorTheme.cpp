#include "EditorTheme.h"

#include <imgui.h>

namespace editor::ui
{
void EditorTheme::ApplyDark()
{
	ImGui::StyleColorsDark();
	ImGuiStyle &Style = ImGui::GetStyle();
	Style.WindowRounding = 7.0f;
	Style.ChildRounding = 7.0f;
	Style.FrameRounding = 7.0f;
	Style.PopupRounding = 8.0f;
	Style.ScrollbarRounding = 9.0f;
	Style.GrabRounding = 7.0f;
	Style.TabRounding = 7.0f;
	Style.FramePadding = ImVec2(7.0f, 5.0f);
	Style.ItemSpacing = ImVec2(7.0f, 5.0f);
	Style.WindowBorderSize = 1.0f;
	Style.FrameBorderSize = 0.0f;
	Style.DockingSeparatorSize = 2.0f;
	Style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.061f, 0.075f, 1.0f);
	Style.Colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.061f, 0.075f, 1.0f);
	Style.Colors[ImGuiCol_PopupBg] = ImVec4(0.065f, 0.072f, 0.087f, 0.98f);
	Style.Colors[ImGuiCol_Border] = ImVec4(0.14f, 0.15f, 0.18f, 1.0f);
	Style.Colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
	Style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
	Style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);
	Style.Colors[ImGuiCol_TitleBg] = ImVec4(0.075f, 0.082f, 0.097f, 1.0f);
	Style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.095f, 0.105f, 0.125f, 1.0f);
	Style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.048f, 0.052f, 0.064f, 1.0f);
	Style.Colors[ImGuiCol_Header] = ImVec4(0.15f, 0.18f, 0.23f, 1.0f);
	Style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.27f, 0.36f, 1.0f);
	Style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.33f, 0.44f, 1.0f);
	Style.Colors[ImGuiCol_Button] = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
	Style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.25f, 0.34f, 1.0f);
	Style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.23f, 0.34f, 0.47f, 1.0f);
	Style.Colors[ImGuiCol_Tab] = ImVec4(0.075f, 0.083f, 0.10f, 1.0f);
	Style.Colors[ImGuiCol_TabHovered] = ImVec4(0.18f, 0.27f, 0.37f, 1.0f);
	Style.Colors[ImGuiCol_TabSelected] = ImVec4(0.13f, 0.19f, 0.27f, 1.0f);
	Style.Colors[ImGuiCol_DockingPreview] = ImVec4(0.20f, 0.48f, 0.82f, 0.75f);
	Style.Colors[ImGuiCol_Separator] = ImVec4(0.14f, 0.15f, 0.18f, 1.0f);
}
} // namespace editor::ui
