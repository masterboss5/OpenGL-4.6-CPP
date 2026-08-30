#pragma once

#include "Source/editor/workspace/EditorWorkspace.h"
#include "Source/pipeline/render/ViewportOverlay.h"
#include "Source/types.h"

#include <filesystem>
#include <span>
#include <vector>

namespace editor::ui
{
struct EditorViewportLayoutState final
{
	uint64 View = 0;
	pipeline::render::ViewportSettings Settings;
};

class EditorLayoutStore final
{
  public:
	explicit EditorLayoutStore(std::filesystem::path Path);

	[[nodiscard]] bool Load(workspace::EditorWorkspace &Workspace, string &DockingState, uint32 CurrentWidth = 0, uint32 CurrentHeight = 0,
							std::vector<EditorViewportLayoutState> *ViewportStates = nullptr);
	void Capture(std::span<const workspace::EditorPanelState> Panels, string_view DockingState, uint32 CurrentWidth = 0,
				 uint32 CurrentHeight = 0, std::span<const EditorViewportLayoutState> ViewportStates = {});
	void Flush();

	[[nodiscard]] const std::filesystem::path &GetPath() const noexcept;
	[[nodiscard]] bool IsDirty() const noexcept;

  private:
	static constexpr uint32 CurrentFormatVersion = 4;

	std::filesystem::path Path;
	std::vector<workspace::EditorPanelState> Panels;
	string DockingState;
	std::vector<EditorViewportLayoutState> ViewportStates;
	uint32 Width = 0;
	uint32 Height = 0;
	bool Dirty = false;
};
} // namespace editor::ui
