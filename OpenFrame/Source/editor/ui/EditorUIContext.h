#pragma once

#include <mutex>

struct ImGuiContext;

namespace editor::ui
{
class EditorUIContext final
{
  public:
	EditorUIContext();
	~EditorUIContext();

	EditorUIContext(const EditorUIContext &) = delete;
	EditorUIContext &operator=(const EditorUIContext &) = delete;
	EditorUIContext(EditorUIContext &&) = delete;
	EditorUIContext &operator=(EditorUIContext &&) = delete;

	void Activate() const noexcept;
	[[nodiscard]] std::mutex &GetMutex() noexcept;
	[[nodiscard]] ImGuiContext *GetNativeContext() const noexcept;

  private:
	ImGuiContext *Context = nullptr;
	std::mutex Mutex;
};
} // namespace editor::ui
