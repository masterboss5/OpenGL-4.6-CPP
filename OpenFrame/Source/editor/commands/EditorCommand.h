#pragma once

#include "Source/types.h"

#include <memory>

namespace editor::commands
{
class EditorCommand
{
  public:
	virtual ~EditorCommand() = default;

	[[nodiscard]] virtual string_view GetName() const noexcept = 0;
	virtual void Execute() = 0;
	virtual void Undo() = 0;
	virtual void Finalize()
	{
	}
	[[nodiscard]] virtual bool TryMerge(const EditorCommand &)
	{
		return false;
	}
};

using EditorCommandPtr = std::unique_ptr<EditorCommand>;
} // namespace editor::commands
