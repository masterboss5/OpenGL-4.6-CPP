#include "EditorWorkspace.h"

#include <array>

namespace editor::workspace
{
namespace
{
constexpr std::array WorkspaceDescriptors{EditorWorkspaceDescriptor{.ID = EditorWorkspaceID::Home, .Name = "Home"},
										  EditorWorkspaceDescriptor{.ID = EditorWorkspaceID::Model, .Name = "Model"},
										  EditorWorkspaceDescriptor{.ID = EditorWorkspaceID::Material, .Name = "Material"},
										  EditorWorkspaceDescriptor{.ID = EditorWorkspaceID::Animation, .Name = "Animation"},
										  EditorWorkspaceDescriptor{.ID = EditorWorkspaceID::Rendering, .Name = "Rendering"},
										  EditorWorkspaceDescriptor{.ID = EditorWorkspaceID::Tools, .Name = "Tools"}};

constexpr EditorWorkspaceVisibility GeneralAuthoring = EditorWorkspaceVisibility::Home | EditorWorkspaceVisibility::Model |
													   EditorWorkspaceVisibility::Material | EditorWorkspaceVisibility::Animation;
constexpr EditorWorkspaceVisibility TransformAuthoring =
	EditorWorkspaceVisibility::Home | EditorWorkspaceVisibility::Model | EditorWorkspaceVisibility::Animation;
constexpr EditorWorkspaceVisibility ModelAuthoring = EditorWorkspaceVisibility::Model;
constexpr EditorWorkspaceVisibility MaterialAuthoring = EditorWorkspaceVisibility::Model | EditorWorkspaceVisibility::Material;
} // namespace

EditorWorkspace::EditorWorkspace()
{
	this->ResetToReferenceLayout();
	this->ToolbarGroups = {
		{.Name = "History", .Order = 10, .Actions = {action::IDs::Undo, action::IDs::Redo}, .VisibleIn = GeneralAuthoring},
		{.Name = "Content", .Order = 15, .Actions = {action::IDs::OpenScene, action::IDs::SaveScene}, .VisibleIn = GeneralAuthoring},
		{.Name = "Transform",
		 .Order = 20,
		 .Actions = {action::IDs::SelectTool, action::IDs::TranslateTool, action::IDs::RotateTool, action::IDs::ScaleTool,
					 action::IDs::UniversalTool, action::IDs::ToggleTransformSpace, action::IDs::ToggleTransformSnapping,
					 action::IDs::CycleTransformPivot},
		 .VisibleIn = TransformAuthoring},
		{.Name = "Primitives",
		 .Order = 25,
		 .Actions = {action::IDs::CreateBox, action::IDs::CreateSphere, action::IDs::CreateCapsule, action::IDs::CreateCylinder,
					 action::IDs::CreateCone, action::IDs::CreatePlane},
		 .VisibleIn = ModelAuthoring},
		{.Name = "Selection",
		 .Order = 27,
		 .Actions = {action::IDs::GroupObjects, action::IDs::ToggleSelectionLocked, action::IDs::MobilityStatic,
					 action::IDs::MobilityStationary, action::IDs::MobilityMovable},
		 .VisibleIn = EditorWorkspaceVisibility::Model | EditorWorkspaceVisibility::Animation},
		{.Name = "Materials",
		 .Order = 28,
		 .Actions = {action::IDs::ToggleMaterialEditor, action::IDs::EditMaterialColor},
		 .VisibleIn = MaterialAuthoring},
		{.Name = "Import",
		 .Order = 29,
		 .Actions = {action::IDs::ImportAssets},
		 .VisibleIn = EditorWorkspaceVisibility::Model | EditorWorkspaceVisibility::Material},
		{.Name = "Play",
		 .Order = 30,
		 .Actions = {action::IDs::Play, action::IDs::Simulate, action::IDs::Pause, action::IDs::Step, action::IDs::Standalone}}};
	this->ToolbarGroups.push_back(
		{.Name = "Build",
		 .Order = 40,
		 .Actions = {action::IDs::BuildGameModule, action::IDs::CancelProjectBuild, action::IDs::CookProject, action::IDs::PackageProject},
		 .VisibleIn = EditorWorkspaceVisibility::Home | EditorWorkspaceVisibility::Tools});
	this->ToolbarGroups.push_back(
		{.Name = "View Modes",
		 .Order = 42,
		 .Actions = {action::IDs::ViewLit, action::IDs::ViewUnlit, action::IDs::ViewWireframe, action::IDs::ViewNormals,
					 action::IDs::ViewDepth, action::IDs::ViewObjectID, action::IDs::ViewOverdraw},
		 .VisibleIn = EditorWorkspaceVisibility::Home | EditorWorkspaceVisibility::Rendering});
	this->ToolbarGroups.push_back(
		{.Name = "Overlays",
		 .Order = 43,
		 .Actions = {action::IDs::OverlayGrid, action::IDs::OverlayBounds, action::IDs::OverlaySkeletons, action::IDs::OverlayCameras,
					 action::IDs::OverlayLights, action::IDs::OverlayCulling, action::IDs::OverlaySelection,
					 action::IDs::OverlayRenderStatistics, action::IDs::OverlayRenderGraph},
		 .VisibleIn = EditorWorkspaceVisibility::Home | EditorWorkspaceVisibility::Rendering});
	this->ToolbarGroups.push_back({.Name = "Panels",
								   .Order = 50,
								   .Actions = {action::IDs::ToggleExplorer, action::IDs::ToggleProperties, action::IDs::ToggleAssetBrowser,
											   action::IDs::ToggleMaterialEditor, action::IDs::ToggleDiagnostics},
								   .VisibleIn = EditorWorkspaceVisibility::Home | EditorWorkspaceVisibility::Tools});
}

void EditorWorkspace::ResetToReferenceLayout()
{
	this->Panels = {{.ID = EditorPanelID::Viewport,
					 .Name = "Viewport",
					 .DefaultRegion = DockRegion::Center,
					 .DefaultSizeRatio = 1.0f,
					 .MinimumWidth = 320.0f,
					 .MinimumHeight = 240.0f,
					 .Open = true,
					 .Minimized = false,
					 .Closable = false,
					 .Resizable = true},
					{.ID = EditorPanelID::Properties,
					 .Name = "Properties",
					 .DefaultRegion = DockRegion::Left,
					 .DefaultSizeRatio = 0.22f,
					 .MinimumWidth = 260.0f},
					{.ID = EditorPanelID::Explorer,
					 .Name = "Explorer",
					 .DefaultRegion = DockRegion::Right,
					 .DefaultSizeRatio = 0.22f,
					 .MinimumWidth = 260.0f},
					{.ID = EditorPanelID::AssetBrowser,
					 .Name = "Assets",
					 .DefaultRegion = DockRegion::Bottom,
					 .DefaultSizeRatio = 0.28f,
					 .MinimumHeight = 160.0f},
					{.ID = EditorPanelID::MaterialEditor,
					 .Name = "Material Editor",
					 .DefaultRegion = DockRegion::Bottom,
					 .DefaultSizeRatio = 0.28f,
					 .MinimumHeight = 220.0f,
					 .Open = false,
					 .Minimized = false},
					{.ID = EditorPanelID::Output,
					 .Name = "Output",
					 .DefaultRegion = DockRegion::Bottom,
					 .DefaultSizeRatio = 0.28f,
					 .MinimumHeight = 140.0f,
					 .Open = false,
					 .Minimized = false},
					{.ID = EditorPanelID::Diagnostics,
					 .Name = "Diagnostics",
					 .DefaultRegion = DockRegion::Bottom,
					 .DefaultSizeRatio = 0.28f,
					 .MinimumHeight = 140.0f,
					 .Open = false,
					 .Minimized = false}};
	++this->LayoutResetGeneration;
}

void EditorWorkspace::SetOpen(const EditorPanelID Panel, const bool Open)
{
	EditorPanelState &State = this->GetPanel(Panel);
	if (!Open && !State.Closable)
		throw EditorWorkspaceException("Panel '" + State.Name + "' cannot be closed");
	State.Open = Open;
	if (!Open)
		State.Minimized = false;
}

void EditorWorkspace::SetMinimized(const EditorPanelID Panel, const bool Minimized)
{
	EditorPanelState &State = this->GetPanel(Panel);
	if (!State.Open && Minimized)
		throw EditorWorkspaceException("A closed panel cannot be minimized");
	State.Minimized = Minimized;
}

void EditorWorkspace::Toggle(const EditorPanelID Panel)
{
	EditorPanelState &State = this->GetPanel(Panel);
	this->SetOpen(Panel, !State.Open);
}

EditorPanelState &EditorWorkspace::GetPanel(const EditorPanelID Panel)
{
	return this->Panels.at(EditorWorkspace::PanelIndex(Panel));
}

const EditorPanelState &EditorWorkspace::GetPanel(const EditorPanelID Panel) const
{
	return this->Panels.at(EditorWorkspace::PanelIndex(Panel));
}

std::span<const EditorPanelState> EditorWorkspace::GetPanels() const noexcept
{
	return this->Panels;
}

std::span<const EditorToolbarGroup> EditorWorkspace::GetToolbarGroups() const noexcept
{
	return this->ToolbarGroups;
}

std::span<const EditorWorkspaceDescriptor> EditorWorkspace::GetWorkspaceDescriptors() noexcept
{
	return WorkspaceDescriptors;
}

uint64 EditorWorkspace::GetLayoutResetGeneration() const noexcept
{
	return this->LayoutResetGeneration;
}

usize EditorWorkspace::PanelIndex(const EditorPanelID Panel)
{
	const usize Index = static_cast<usize>(Panel);
	if (Index >= static_cast<usize>(EditorPanelID::Count))
		throw EditorWorkspaceException("Editor panel identity is invalid");
	return Index;
}
} // namespace editor::workspace
