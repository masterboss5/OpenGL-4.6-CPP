#pragma once

#include "Source/types.h"

namespace editor::ui
{
class EditorDockspace final
{
  public:
	static void BuildReferenceLayout(uint32 DockspaceID, float32 Width, float32 Height);
	static void ResizeReferenceLayoutIfUnmodified(uint32 DockspaceID, float32 Width, float32 Height);
};
} // namespace editor::ui
