#pragma once

#include "src/types.h"

#include <unordered_map>

namespace editor::ui
{
class EditorIconRegistry final
{
  public:
	EditorIconRegistry();

	[[nodiscard]] string_view Find(string_view Name) const noexcept;

  private:
	std::unordered_map<string, string> Icons;
};
} // namespace editor::ui
