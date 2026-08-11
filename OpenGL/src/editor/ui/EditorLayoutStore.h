#pragma once

#include "src/editor/workspace/EditorWorkspace.h"
#include "src/pipeline/render/ViewportOverlay.h"
#include "src/types.h"

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
	static constexpr uint32 CurrentFormatVersion = 3;

	std::filesystem::path Path;
	std::vector<workspace::EditorPanelState> Panels;
	string DockingState;
	std::vector<EditorViewportLayoutState> ViewportStates;
	uint32 Width = 0;
	uint32 Height = 0;
	bool Dirty = false;
};
} // namespace editor::ui
