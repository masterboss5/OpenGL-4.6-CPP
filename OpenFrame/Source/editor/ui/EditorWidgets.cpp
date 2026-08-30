#include "EditorWidgets.h"

#include "EditorIconRegistry.h"
#include "EditorTheme.h"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace editor::ui
{
namespace
{
[[nodiscard]] string_view VisibleLabel(const string_view Label) noexcept
{
	const usize HiddenMarker = Label.find("##");
	return HiddenMarker == string_view::npos ? Label : Label.substr(0, HiddenMarker);
}

struct TextLineMetrics final
{
	ImVec2 Size{};
	float32 GlyphMinimumY = 0.0F;
	float32 GlyphMaximumY = 0.0F;
	float32 SlotHeight = 0.0F;
};

[[nodiscard]] TextLineMetrics MeasureTextLine(const char *const Begin, const char *const End)
{
	TextLineMetrics Metrics{.Size = ImGui::CalcTextSize(Begin, End, false)};
	const float32 LineHeight = ImGui::GetFontSize();
	Metrics.GlyphMinimumY = std::numeric_limits<float32>::max();
	Metrics.GlyphMaximumY = std::numeric_limits<float32>::lowest();
	ImFontBaked *const Font = ImGui::GetFontBaked();
	const char *Cursor = Begin;
	while (Cursor < End)
	{
		uint32 Codepoint = 0;
		const int32 ByteCount = ImTextCharFromUtf8(&Codepoint, Cursor, End);
		if (ByteCount <= 0)
			break;
		Cursor += ByteCount;
		const ImFontGlyph *const Glyph = Font == nullptr ? nullptr : Font->FindGlyphNoFallback(static_cast<ImWchar>(Codepoint));
		if (Glyph == nullptr || !Glyph->Visible)
			continue;
		Metrics.GlyphMinimumY = std::min(Metrics.GlyphMinimumY, Glyph->Y0);
		Metrics.GlyphMaximumY = std::max(Metrics.GlyphMaximumY, Glyph->Y1);
	}
	if (Metrics.GlyphMinimumY == std::numeric_limits<float32>::max())
	{
		Metrics.GlyphMinimumY = 0.0F;
		Metrics.GlyphMaximumY = LineHeight;
	}
	Metrics.SlotHeight = std::max(LineHeight, Metrics.GlyphMaximumY - Metrics.GlyphMinimumY);
	return Metrics;
}

[[nodiscard]] ImVec2 MeasureButtonLabel(const string_view Label)
{
	float32 MaximumWidth = 0.0F;
	float32 TotalHeight = 0.0F;
	const char *LineBegin = Label.data();
	const char *const LabelEnd = Label.data() + Label.size();
	do
	{
		const char *LineEnd = LineBegin;
		while (LineEnd != LabelEnd && *LineEnd != '\n')
			++LineEnd;
		const TextLineMetrics Metrics = MeasureTextLine(LineBegin, LineEnd);
		MaximumWidth = std::max(MaximumWidth, Metrics.Size.x);
		TotalHeight += Metrics.SlotHeight;
		LineBegin = LineEnd == LabelEnd ? LabelEnd : LineEnd + 1;
	} while (LineBegin != LabelEnd);
	return ImVec2(MaximumWidth, TotalHeight);
}

[[nodiscard]] ImVec2 ResolveButtonSize(const char *Label, ImVec2 Requested, const float32 DefaultHeight, const bool AllowLabelClipping)
{
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	const string_view Visible = VisibleLabel(Label);
	const ImVec2 TextSize = MeasureButtonLabel(Visible);
	const float32 LabelWidth = TextSize.x + Tokens.FramePadding.x * 2.0F;
	if (Requested.x == 0.0F)
		Requested.x = LabelWidth;
	else if (Requested.x < 0.0F)
		Requested.x = std::max(1.0F, ImGui::GetContentRegionAvail().x + Requested.x + 1.0F);
	else if (!AllowLabelClipping)
		Requested.x = std::max(Requested.x, LabelWidth);
	if (Requested.y <= 0.0F)
		Requested.y = DefaultHeight;
	Requested.y = std::max(Requested.y, TextSize.y + Tokens.FramePadding.y * 2.0F);
	return Requested;
}

[[nodiscard]] bool DrawButtonLabel(ImDrawList &DrawList, const string_view Label, const ImVec2 Minimum, const ImVec2 Maximum,
								   const ImVec4 Color)
{
	if (Label.empty())
		return false;
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	const ImVec2 InnerMinimum(Minimum.x + Tokens.FramePadding.x, Minimum.y + Tokens.FramePadding.y);
	const ImVec2 InnerMaximum(std::max(InnerMinimum.x + 1.0F, Maximum.x - Tokens.FramePadding.x),
							  std::max(InnerMinimum.y + 1.0F, Maximum.y - Tokens.FramePadding.y));
	const float32 InnerWidth = std::max(0.0F, InnerMaximum.x - InnerMinimum.x);
	const float32 InnerHeight = std::max(0.0F, InnerMaximum.y - InnerMinimum.y);
	usize LineCount = 1;
	for (const char Character : Label)
		LineCount += Character == '\n' ? 1U : 0U;
	float32 TextBlockHeight = 0.0F;
	const char *MeasurementBegin = Label.data();
	const char *const LabelEnd = Label.data() + Label.size();
	for (usize LineIndex = 0; LineIndex < LineCount; ++LineIndex)
	{
		const char *MeasurementEnd = MeasurementBegin;
		while (MeasurementEnd != LabelEnd && *MeasurementEnd != '\n')
			++MeasurementEnd;
		TextBlockHeight += MeasureTextLine(MeasurementBegin, MeasurementEnd).SlotHeight;
		MeasurementBegin = MeasurementEnd == LabelEnd ? LabelEnd : MeasurementEnd + 1;
	}
	float32 LineY = InnerMinimum.y + std::max(0.0F, (InnerHeight - TextBlockHeight) * 0.5F);
	bool Clipped = TextBlockHeight > InnerHeight;
	const char *LineBegin = Label.data();
	const ImU32 PackedColor = ImGui::GetColorU32(Color);
	const ImVec4 ClipRect(InnerMinimum.x, InnerMinimum.y, InnerMaximum.x, InnerMaximum.y);
	DrawList.PushClipRect(InnerMinimum, InnerMaximum, true);
	for (usize LineIndex = 0; LineIndex < LineCount; ++LineIndex)
	{
		const char *LineEnd = LineBegin;
		while (LineEnd != LabelEnd && *LineEnd != '\n')
			++LineEnd;
		const TextLineMetrics Metrics = MeasureTextLine(LineBegin, LineEnd);
		const bool LineClipped = Metrics.Size.x > InnerWidth;
		Clipped = Clipped || LineClipped;
		const float32 LineX = InnerMinimum.x + std::max(0.0F, (InnerWidth - Metrics.Size.x) * 0.5F);
		const float32 TextY = LineY + (Metrics.SlotHeight - (Metrics.GlyphMaximumY - Metrics.GlyphMinimumY)) * 0.5F - Metrics.GlyphMinimumY;
		if (LineClipped)
		{
			ImGui::RenderTextEllipsis(&DrawList, ImVec2(LineX, TextY), InnerMaximum, InnerMaximum.x, LineBegin, LineEnd, &Metrics.Size);
		}
		else
		{
			DrawList.AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(LineX, TextY), PackedColor, LineBegin, LineEnd, 0.0F,
							 &ClipRect);
		}
		LineY += Metrics.SlotHeight;
		LineBegin = LineEnd == LabelEnd ? LabelEnd : LineEnd + 1;
	}
	DrawList.PopClipRect();
	return Clipped;
}

void DrawFocusRing(const ImVec2 Minimum, const ImVec2 Maximum, const float32 Rounding)
{
	if (!ImGui::IsItemFocused())
		return;
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	ImGui::GetWindowDrawList()->AddRect(ImVec2(Minimum.x - 1.0F, Minimum.y - 1.0F), ImVec2(Maximum.x + 1.0F, Maximum.y + 1.0F),
										ImGui::GetColorU32(Tokens.FocusRing), Rounding + 1.0F, 0, Tokens.FocusWidth);
}
} // namespace

bool EditorWidgets::Button(const char *Label, const ImVec2 Size, const EditorControlRole Role, const bool Selected)
{
	return Button(Label, EditorButtonOptions{.Size = Size, .Role = Role, .Selected = Selected});
}

bool EditorWidgets::Button(const char *Label, const EditorButtonOptions &Options)
{
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	const bool ParentDisabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
	const bool Enabled = Options.Enabled && !ParentDisabled;
	const ImVec2 Size = ResolveButtonSize(Label, Options.Size, Tokens.ControlHeight, Options.AllowLabelClipping);
	if (!Options.Enabled)
		ImGui::BeginDisabled();
	const bool Activated = ImGui::InvisibleButton(Label, Size);
	if (!Options.Enabled)
		ImGui::EndDisabled();
	const ImVec2 Minimum = ImGui::GetItemRectMin();
	const ImVec2 Maximum = ImGui::GetItemRectMax();
	const bool Hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
	const bool Pressed = ImGui::IsItemActive();
	const EditorInteractionState State = EditorStyleSystem::ResolveState(Options.Selected, Hovered, Pressed, Enabled);
	const EditorControlVisual Visual = EditorStyleSystem::Resolve(Options.Role, State);
	ImDrawList *const DrawList = ImGui::GetWindowDrawList();
	const float32 Rounding = Tokens.ButtonRounding;
	if (Visual.Fill.w > 0.0F)
	{
		DrawList->AddRectFilled(ImVec2(Minimum.x, Minimum.y + 1.0F), ImVec2(Maximum.x, Maximum.y + 1.0F), ImGui::GetColorU32(Tokens.Shadow),
								Rounding);
		DrawList->AddRectFilled(Minimum, Maximum, ImGui::GetColorU32(Visual.Fill), Rounding);
		DrawList->AddLine(ImVec2(Minimum.x + Rounding, Minimum.y + 1.0F), ImVec2(Maximum.x - Rounding, Minimum.y + 1.0F),
						  ImGui::GetColorU32(Tokens.HighlightLine), Tokens.SignatureHighlightWidth);
	}
	if (Visual.Border.w > 0.0F)
		DrawList->AddRect(Minimum, Maximum, ImGui::GetColorU32(Visual.Border), Rounding, 0, Tokens.BorderWidth);
	if (Visual.DrawAccent)
	{
		const float32 RailInset = 5.0F;
		DrawList->AddRectFilled(ImVec2(Minimum.x + 2.0F, Minimum.y + RailInset),
								ImVec2(Minimum.x + 2.0F + Tokens.SignatureRailWidth, Maximum.y - RailInset),
								ImGui::GetColorU32(Visual.Accent), Tokens.SignatureRailWidth * 0.5F);
	}
	const string_view Visible = VisibleLabel(Label);
	const bool LabelClipped = DrawButtonLabel(*DrawList, Visible, Minimum, Maximum, Visual.Text);
	DrawFocusRing(Minimum, Maximum, Rounding);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && (!Options.Tooltip.empty() || LabelClipped))
	{
		const string_view Tooltip = Options.Tooltip.empty() ? Visible : Options.Tooltip;
		ImGui::SetTooltip("%.*s", static_cast<int32>(Tooltip.size()), Tooltip.data());
	}
	return Enabled && Activated;
}

bool EditorWidgets::SmallButton(const char *Label, const EditorControlRole Role, const bool Selected)
{
	return Button(Label, ImVec2(0.0F, EditorTheme::GetTokens().CompactControlHeight), Role, Selected);
}

bool EditorWidgets::Selectable(const char *Label, const bool Selected, const ImGuiSelectableFlags Flags, const ImVec2 Size)
{
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	const bool Enabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) == 0;
	const EditorControlVisual Idle =
		EditorStyleSystem::Resolve(EditorControlRole::Row, EditorStyleSystem::ResolveState(Selected, false, false, Enabled));
	const EditorControlVisual Hover =
		EditorStyleSystem::Resolve(EditorControlRole::Row, EditorStyleSystem::ResolveState(Selected, true, false, Enabled));
	const EditorControlVisual Press =
		EditorStyleSystem::Resolve(EditorControlRole::Row, EditorStyleSystem::ResolveState(Selected, true, true, Enabled));
	ImGui::PushStyleColor(ImGuiCol_Header, Idle.Fill);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Hover.Fill);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, Press.Fill);
	ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0F, 0.5F));
	const bool Activated = ImGui::Selectable(Label, Selected, Flags, Size);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(3);
	const ImVec2 Minimum = ImGui::GetItemRectMin();
	const ImVec2 Maximum = ImGui::GetItemRectMax();
	if (Selected)
	{
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(Minimum.x + 2.0F, Minimum.y + 5.0F),
												  ImVec2(Minimum.x + 2.0F + Tokens.SignatureRailWidth, Maximum.y - 5.0F),
												  ImGui::GetColorU32(Tokens.Accent), Tokens.SignatureRailWidth * 0.5F);
	}
	DrawFocusRing(Minimum, Maximum, Tokens.TreeRowRounding);
	return Activated;
}

bool EditorWidgets::Checkbox(const char *Label, bool *Value)
{
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	ImGui::PushStyleColor(ImGuiCol_FrameBg, Tokens.SurfaceInset);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Tokens.SurfaceHover);
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Tokens.SurfacePressed);
	ImGui::PushStyleColor(ImGuiCol_CheckMark, Tokens.TextPrimary);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0F);
	const bool Changed = ImGui::Checkbox(Label, Value);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
	DrawFocusRing(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), 5.0F);
	return Changed;
}

bool EditorWidgets::CollapsingHeader(const char *Label, const ImGuiTreeNodeFlags Flags)
{
	return CollapsingHeader(Label, nullptr, Flags);
}

bool EditorWidgets::CollapsingHeader(const char *Label, bool *Visible, const ImGuiTreeNodeFlags Flags)
{
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	ImGui::PushStyleColor(ImGuiCol_Header, Tokens.PanelRaised);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Tokens.SurfaceHover);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, Tokens.SurfacePressed);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Tokens.TreeRowRounding);
	const bool Open = Visible == nullptr ? ImGui::CollapsingHeader(Label, Flags) : ImGui::CollapsingHeader(Label, Visible, Flags);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(3);
	if (Open)
	{
		const ImVec2 Minimum = ImGui::GetItemRectMin();
		const ImVec2 Maximum = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(Minimum.x + 2.0F, Minimum.y + 5.0F),
												  ImVec2(Minimum.x + 2.0F + Tokens.SignatureRailWidth, Maximum.y - 5.0F),
												  ImGui::GetColorU32(Tokens.Accent), Tokens.SignatureRailWidth * 0.5F);
	}
	return Open;
}

bool EditorWidgets::InputText(const char *Label, string &Value)
{
	return ImGui::InputText(Label, &Value);
}

bool EditorWidgets::SearchField(const char *Label, string &Value, const EditorIconRegistry &Icons, const string_view Hint)
{
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	const string_view SearchIcon = Icons.Find("Search");
	ImGui::BeginGroup();
	ImGui::PushID(Label);
	if (!SearchIcon.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Tokens.TextMuted);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(SearchIcon.data(), SearchIcon.data() + SearchIcon.size());
		ImGui::PopStyleColor();
		ImGui::SameLine(0.0F, Tokens.IconLabelGap);
	}
	const float32 ClearWidth = Value.empty() ? 0.0F : Tokens.CompactControlHeight + Tokens.ItemInnerSpacing.x;
	ImGui::SetNextItemWidth(std::max(1.0F, ImGui::GetContentRegionAvail().x - ClearWidth));
	const bool Changed = ImGui::InputTextWithHint("##SearchInput", string(Hint).c_str(), &Value);
	if (!Value.empty())
	{
		ImGui::SameLine(0.0F, Tokens.ItemInnerSpacing.x);
		if (Button("x##ClearSearch", ImVec2(Tokens.CompactControlHeight, Tokens.CompactControlHeight), EditorControlRole::Quiet))
			Value.clear();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Clear search");
	}
	ImGui::PopID();
	ImGui::EndGroup();
	return Changed;
}

EditorProjectCardResult EditorWidgets::ProjectCard(const char *const ID, const EditorProjectCardOptions &Options)
{
	if (ID == nullptr || Options.Name.empty())
		throw std::invalid_argument("A project card requires a stable identity and visible project name");
	const EditorThemeTokens &Tokens = EditorTheme::GetTokens();
	const ImVec2 Size(std::max(1.0F, Options.Size.x), std::max(Tokens.HomeProjectCardHeight, Options.Size.y));
	const bool Activated = ImGui::InvisibleButton(ID, Size, ImGuiButtonFlags_EnableNav);
	const ImVec2 Minimum = ImGui::GetItemRectMin();
	const ImVec2 Maximum = ImGui::GetItemRectMax();
	const bool Hovered = ImGui::IsItemHovered();
	const bool Pressed = ImGui::IsItemActive();
	ImDrawList *const DrawList = ImGui::GetWindowDrawList();
	const ImVec4 Fill = !Options.Available ? Tokens.SurfaceInset
						: Pressed		   ? Tokens.SurfacePressed
						: Hovered		   ? Tokens.SurfaceHover
										   : Tokens.PanelRaised;
	const ImVec4 Border = Hovered && Options.Available ? Tokens.Accent : Tokens.BorderSubtle;
	DrawList->AddRectFilled(ImVec2(Minimum.x, Minimum.y + 3.0F), ImVec2(Maximum.x, Maximum.y + 3.0F), ImGui::GetColorU32(Tokens.Shadow),
							Tokens.CardRounding);
	DrawList->AddRectFilled(Minimum, Maximum, ImGui::GetColorU32(Fill), Tokens.CardRounding);
	const float32 ThumbnailHeight = std::min(Tokens.HomeProjectThumbnailHeight, Size.y - 58.0F);
	const ImVec2 ImageMinimum(Minimum.x + Tokens.BorderWidth, Minimum.y + Tokens.BorderWidth);
	const ImVec2 ImageMaximum(Maximum.x - Tokens.BorderWidth, Minimum.y + ThumbnailHeight);
	if (Options.Thumbnail._TexData != nullptr || Options.Thumbnail.GetTexID() != ImTextureID_Invalid)
	{
		DrawList->AddImageRounded(Options.Thumbnail, ImageMinimum, ImageMaximum, ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F), IM_COL32_WHITE,
								  std::max(0.0F, Tokens.CardRounding - Tokens.BorderWidth), ImDrawFlags_RoundCornersTop);
	}
	else
	{
		DrawList->AddRectFilled(ImageMinimum, ImageMaximum, ImGui::GetColorU32(Tokens.SurfaceInset),
								Tokens.CardRounding - Tokens.BorderWidth, ImDrawFlags_RoundCornersTop);
	}
	if (!Options.Available)
		DrawList->AddRectFilled(ImageMinimum, ImageMaximum, ImGui::GetColorU32(ImVec4(0.0F, 0.0F, 0.0F, 0.48F)),
								Tokens.CardRounding - Tokens.BorderWidth, ImDrawFlags_RoundCornersTop);
	if (Hovered && Options.Available)
		DrawList->AddRectFilled(ImageMinimum, ImageMaximum,
								ImGui::GetColorU32(ImVec4(Tokens.Accent.x, Tokens.Accent.y, Tokens.Accent.z, 0.08F)),
								Tokens.CardRounding - Tokens.BorderWidth, ImDrawFlags_RoundCornersTop);
	DrawList->AddLine(ImVec2(Minimum.x + Tokens.CardRounding, ImageMaximum.y), ImVec2(Maximum.x - Tokens.CardRounding, ImageMaximum.y),
					  ImGui::GetColorU32(Tokens.HighlightLine), Tokens.SignatureHighlightWidth);
	DrawList->AddRect(Minimum, Maximum, ImGui::GetColorU32(Border), Tokens.CardRounding, 0, Tokens.BorderWidth);

	const float32 TextLeft = Minimum.x + Tokens.WindowPadding.x;
	const float32 TextRight = Maximum.x - Tokens.WindowPadding.x;
	const float32 NameY = ImageMaximum.y + Tokens.ItemSpacing.y;
	ImGui::PushStyleColor(ImGuiCol_Text, Options.Available ? Tokens.TextPrimary : Tokens.TextDisabled);
	ImGui::RenderTextEllipsis(DrawList, ImVec2(TextLeft, NameY), ImVec2(TextRight, NameY + ImGui::GetFontSize()), TextRight,
							  Options.Name.data(), Options.Name.data() + Options.Name.size(), nullptr);
	ImGui::PopStyleColor();
	const string_view Metadata = Options.Available ? Options.LastEdited : string_view("Project unavailable");
	ImGui::PushStyleColor(ImGuiCol_Text, Options.Available ? Tokens.TextSecondary : Tokens.Danger);
	ImGui::RenderTextEllipsis(DrawList, ImVec2(TextLeft, NameY + ImGui::GetFontSize() + Tokens.ItemInnerSpacing.y * 0.5F),
							  ImVec2(TextRight, Maximum.y - Tokens.WindowPadding.y), TextRight, Metadata.data(),
							  Metadata.data() + Metadata.size(), nullptr);
	ImGui::PopStyleColor();
	DrawFocusRing(Minimum, Maximum, Tokens.CardRounding);

	EditorProjectCardResult Result{.OpenRequested = Options.Available && Activated};
	if (ImGui::BeginPopupContextItem("ProjectCardContext"))
	{
		ImGui::TextDisabled("Recent project");
		ImGui::Separator();
		if (ImGui::MenuItem("Remove from recent"))
			Result.RemoveRequested = true;
		ImGui::EndPopup();
	}
	if (Hovered && !Options.DescriptorPath.empty())
		ImGui::SetTooltip("%.*s", static_cast<int32>(Options.DescriptorPath.size()), Options.DescriptorPath.data());
	return Result;
}
} // namespace editor::ui
