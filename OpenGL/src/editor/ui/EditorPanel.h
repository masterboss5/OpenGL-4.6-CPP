#pragma once

#include "src/editor/workspace/EditorWorkspace.h"

namespace editor
{
class EditorSession;
}

namespace editor::ui
{
class EditorPanel
{
  public:
	virtual ~EditorPanel() = default;

	[[nodiscard]] workspace::EditorPanelID GetID() const noexcept;
	[[nodiscard]] virtual bool Render(EditorSession &Session) = 0;

  protected:
	explicit EditorPanel(workspace::EditorPanelID ID) noexcept;

  private:
	workspace::EditorPanelID ID;
};
} // namespace editor::ui
