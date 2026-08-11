#pragma once

#include "src/editor/action/EditorActionRegistry.h"
#include "src/types.h"

#include <span>
#include <stdexcept>
#include <vector>

namespace editor::workspace
{
enum class EditorPanelID : uint8
{
	Viewport,
	Properties,
	Explorer,
	AssetBrowser,
	MaterialEditor,
	Output,
	Diagnostics,
	Count
};

enum class DockRegion : uint8
{
	Center,
	Left,
	Right,
	Bottom
};

enum class EditorWorkspaceID : uint8
{
	Home,
	Model,
	Material,
	Animation,
	Rendering,
	Tools,
	Count
};

enum class EditorWorkspaceVisibility : uint8
{
	None = 0,
	Home = 1U << 0U,
	Model = 1U << 1U,
	Material = 1U << 2U,
	Animation = 1U << 3U,
	Rendering = 1U << 4U,
	Tools = 1U << 5U,
	All = (1U << 6U) - 1U
};

[[nodiscard]] constexpr EditorWorkspaceVisibility operator|(const EditorWorkspaceVisibility Left,
															const EditorWorkspaceVisibility Right) noexcept
{
	return static_cast<EditorWorkspaceVisibility>(static_cast<uint8>(Left) | static_cast<uint8>(Right));
}

[[nodiscard]] constexpr bool IsVisible(const EditorWorkspaceVisibility Visibility, const EditorWorkspaceID Workspace) noexcept
{
	if (Workspace >= EditorWorkspaceID::Count)
		return false;
	const uint8 WorkspaceBit = static_cast<uint8>(1U << static_cast<uint8>(Workspace));
	return (static_cast<uint8>(Visibility) & WorkspaceBit) != 0U;
}

struct EditorWorkspaceDescriptor final
{
	EditorWorkspaceID ID = EditorWorkspaceID::Home;
	string_view Name;
};

struct EditorPanelState final
{
	EditorPanelID ID = EditorPanelID::Viewport;
	string Name;
	DockRegion DefaultRegion = DockRegion::Center;
	float32 DefaultSizeRatio = 1.0f;
	float32 MinimumWidth = 160.0f;
	float32 MinimumHeight = 100.0f;
	bool Open = true;
	bool Minimized = false;
	bool Closable = true;
	bool Resizable = true;
};

struct EditorToolbarGroup final
{
	string Name;
	int32 Order = 0;
	std::vector<action::EditorActionID> Actions;
	EditorWorkspaceVisibility VisibleIn = EditorWorkspaceVisibility::All;
};

class EditorWorkspaceException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class EditorWorkspace final
{
  public:
	EditorWorkspace();

	void ResetToReferenceLayout();
	void SetOpen(EditorPanelID Panel, bool Open);
	void SetMinimized(EditorPanelID Panel, bool Minimized);
	void Toggle(EditorPanelID Panel);

	[[nodiscard]] EditorPanelState &GetPanel(EditorPanelID Panel);
	[[nodiscard]] const EditorPanelState &GetPanel(EditorPanelID Panel) const;
	[[nodiscard]] std::span<const EditorPanelState> GetPanels() const noexcept;
	[[nodiscard]] std::span<const EditorToolbarGroup> GetToolbarGroups() const noexcept;
	[[nodiscard]] static std::span<const EditorWorkspaceDescriptor> GetWorkspaceDescriptors() noexcept;
	[[nodiscard]] uint64 GetLayoutResetGeneration() const noexcept;

  private:
	[[nodiscard]] static usize PanelIndex(EditorPanelID Panel);

	std::vector<EditorPanelState> Panels;
	std::vector<EditorToolbarGroup> ToolbarGroups;
	uint64 LayoutResetGeneration = 0;
};
} // namespace editor::workspace
