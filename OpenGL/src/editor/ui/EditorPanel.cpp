#include "EditorPanel.h"

namespace editor::ui
{
EditorPanel::EditorPanel(const workspace::EditorPanelID ID) noexcept : ID(ID)
{
}

workspace::EditorPanelID EditorPanel::GetID() const noexcept
{
	return this->ID;
}
} // namespace editor::ui
