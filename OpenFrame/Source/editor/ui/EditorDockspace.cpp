#include "EditorDockspace.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>

namespace editor::ui
{
namespace
{
constexpr float32 SidePanelWidthRatio = 0.22f;
constexpr float32 BottomPanelHeightRatio = 0.28f;
constexpr float32 ReferenceRatioTolerance = 0.035f;
constexpr float32 ResizeTolerance = 1.0f;

[[nodiscard]] bool IsNear(const float32 Value, const float32 Expected, const float32 Tolerance)
{
	return std::abs(Value - Expected) <= Tolerance;
}

[[nodiscard]] ImGuiDockNode *GetSmallerChild(ImGuiDockNode *const Parent, const ImGuiAxis Axis)
{
	if (Parent == nullptr || Parent->ChildNodes[0] == nullptr || Parent->ChildNodes[1] == nullptr)
		return nullptr;
	const float32 FirstExtent = Axis == ImGuiAxis_X ? Parent->ChildNodes[0]->Size.x : Parent->ChildNodes[0]->Size.y;
	const float32 SecondExtent = Axis == ImGuiAxis_X ? Parent->ChildNodes[1]->Size.x : Parent->ChildNodes[1]->Size.y;
	return FirstExtent <= SecondExtent ? Parent->ChildNodes[0] : Parent->ChildNodes[1];
}

[[nodiscard]] ImGuiDockNode *GetLargerChild(ImGuiDockNode *const Parent, const ImGuiAxis Axis)
{
	if (Parent == nullptr || Parent->ChildNodes[0] == nullptr || Parent->ChildNodes[1] == nullptr)
		return nullptr;
	const float32 FirstExtent = Axis == ImGuiAxis_X ? Parent->ChildNodes[0]->Size.x : Parent->ChildNodes[0]->Size.y;
	const float32 SecondExtent = Axis == ImGuiAxis_X ? Parent->ChildNodes[1]->Size.x : Parent->ChildNodes[1]->Size.y;
	return FirstExtent > SecondExtent ? Parent->ChildNodes[0] : Parent->ChildNodes[1];
}
} // namespace

void EditorDockspace::BuildReferenceLayout(const uint32 DockspaceID, const float32 Width, const float32 Height)
{
	ImGui::DockBuilderRemoveNode(DockspaceID);
	ImGui::DockBuilderAddNode(DockspaceID, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(DockspaceID, ImVec2(Width, Height));
	ImGuiID Center = DockspaceID;
	const ImGuiID Left = ImGui::DockBuilderSplitNode(Center, ImGuiDir_Left, SidePanelWidthRatio, nullptr, &Center);
	const float32 RightPanelRemainingWidthRatio = SidePanelWidthRatio / (1.0f - SidePanelWidthRatio);
	const ImGuiID Right = ImGui::DockBuilderSplitNode(Center, ImGuiDir_Right, RightPanelRemainingWidthRatio, nullptr, &Center);
	const ImGuiID Bottom = ImGui::DockBuilderSplitNode(Center, ImGuiDir_Down, BottomPanelHeightRatio, nullptr, &Center);
	if (ImGuiDockNode *const CenterNode = ImGui::DockBuilderGetNode(Center))
	{
		CenterNode->SetLocalFlags(CenterNode->LocalFlags | ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoWindowMenuButton |
								  ImGuiDockNodeFlags_NoCloseButton | ImGuiDockNodeFlags_NoDockingOverMe);
	}
	ImGui::DockBuilderDockWindow("Properties", Left);
	ImGui::DockBuilderDockWindow("Explorer", Right);
	ImGui::DockBuilderDockWindow("Viewport", Center);
	ImGui::DockBuilderDockWindow("Assets", Bottom);
	ImGui::DockBuilderDockWindow("Material Editor", Bottom);
	ImGui::DockBuilderDockWindow("Output", Bottom);
	ImGui::DockBuilderDockWindow("Diagnostics", Bottom);
	ImGui::DockBuilderFinish(DockspaceID);
}

void EditorDockspace::ResizeReferenceLayoutIfUnmodified(const uint32 DockspaceID, const float32 Width, const float32 Height)
{
	ImGuiDockNode *const Root = ImGui::DockBuilderGetNode(DockspaceID);
	if (Root == nullptr)
		return;

	const bool SizeChanged = !IsNear(Root->Size.x, Width, ResizeTolerance) || !IsNear(Root->Size.y, Height, ResizeTolerance);
	if (!SizeChanged)
		return;

	ImGuiDockNode *const Left = GetSmallerChild(Root, ImGuiAxis_X);
	ImGuiDockNode *const RightSplit = GetLargerChild(Root, ImGuiAxis_X);
	ImGuiDockNode *const Right = GetSmallerChild(RightSplit, ImGuiAxis_X);
	ImGuiDockNode *const CenterSplit = GetLargerChild(RightSplit, ImGuiAxis_X);
	ImGuiDockNode *const Bottom = GetSmallerChild(CenterSplit, ImGuiAxis_Y);
	ImGuiDockNode *const Center = GetLargerChild(CenterSplit, ImGuiAxis_Y);

	const bool HasReferenceTopology = Left != nullptr && RightSplit != nullptr && CenterSplit != nullptr && Right != nullptr &&
									  Center != nullptr && Bottom != nullptr && Root->SplitAxis == ImGuiAxis_X &&
									  RightSplit->SplitAxis == ImGuiAxis_X && CenterSplit->SplitAxis == ImGuiAxis_Y && Left->IsLeafNode() &&
									  Right->IsLeafNode() && Center->IsLeafNode() && Bottom->IsLeafNode();
	if (!HasReferenceTopology || Root->Size.x <= 0.0f || Root->Size.y <= 0.0f || CenterSplit->Size.y <= 0.0f)
	{
		ImGui::DockBuilderSetNodeSize(DockspaceID, ImVec2(Width, Height));
		return;
	}

	const float32 LeftRatio = Left->Size.x / Root->Size.x;
	const float32 RightRatio = Right->Size.x / Root->Size.x;
	const float32 BottomRatio = Bottom->Size.y / CenterSplit->Size.y;
	const bool IsUnmodifiedReferenceLayout = IsNear(LeftRatio, SidePanelWidthRatio, ReferenceRatioTolerance) &&
											 IsNear(RightRatio, SidePanelWidthRatio, ReferenceRatioTolerance) &&
											 IsNear(BottomRatio, BottomPanelHeightRatio, ReferenceRatioTolerance);
	if (IsUnmodifiedReferenceLayout)
	{
		BuildReferenceLayout(DockspaceID, Width, Height);
		return;
	}

	ImGui::DockBuilderSetNodeSize(DockspaceID, ImVec2(Width, Height));
}

} // namespace editor::ui
