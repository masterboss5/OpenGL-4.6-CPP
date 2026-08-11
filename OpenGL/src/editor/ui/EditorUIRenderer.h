#pragma once

struct ImDrawData;

namespace core
{
class Context;
}

namespace editor::ui
{
class EditorUIContext;

class EditorUIRenderer final
{
  public:
	explicit EditorUIRenderer(EditorUIContext &Context) noexcept;
	~EditorUIRenderer();

	EditorUIRenderer(const EditorUIRenderer &) = delete;
	EditorUIRenderer &operator=(const EditorUIRenderer &) = delete;
	EditorUIRenderer(EditorUIRenderer &&) = delete;
	EditorUIRenderer &operator=(EditorUIRenderer &&) = delete;

	void Initialize();
	void Shutdown() noexcept;
	void BeginFrame();
	void Render(ImDrawData &DrawData);
	void Render(ImDrawData &DrawData, core::Context &CurrentContext);
	[[nodiscard]] bool IsInitialized() const noexcept;

  private:
	EditorUIContext *Context = nullptr;
	bool Initialized = false;
};
} // namespace editor::ui
