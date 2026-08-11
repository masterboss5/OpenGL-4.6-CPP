#pragma once

#include "src/core/layers/ApplicationLayer.h"
#include "src/core/input/InputTypes.h"
#include "src/pipeline/render/Renderer.h"
#include "src/scene/Camera.h"
#include "src/types.h"

#include <memory>
#include <span>
#include <vector>

namespace core
{
class Window;
class WindowManager;
namespace diagnostics
{
class DiagnosticSink;
}
} // namespace core

namespace editor
{
class EditorSession;
namespace action
{
class EditorActionRegistry;
struct EditorActionContext;
} // namespace action

namespace ui
{
struct EditorViewportRegion final
{
	pipeline::render::RenderViewID View;
	core::WindowID Window;
	float32 Left = 0.0f;
	float32 Top = 0.0f;
	float32 Width = 0.0f;
	float32 Height = 0.0f;
	core::WindowExtent PixelExtent;
	pipeline::render::ViewportSettings Settings{.Overlays = {.Grid = true}};
	bool Hovered = false;
	bool Focused = false;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool Contains(float64 X, float64 Y) const noexcept;
};

struct EditorViewportPresentation final
{
	pipeline::render::RenderViewID View;
	string Name;
	pipeline::render::RenderViewOutput Output;
	CameraProjectionMode Projection = CameraProjectionMode::Perspective;
	bool Closable = true;
};

struct EditorViewportTextureBinding final
{
	pipeline::render::RenderViewID View;
	pipeline::graph::ExportedTexture Texture;
};

namespace detail
{
struct EditorUIDrawData;
}

struct EditorUIWindowFrame final
{
	core::WindowID Window;
	core::Window *ManagedWindow = nullptr;
	bool Main = false;
	std::shared_ptr<detail::EditorUIDrawData> DrawData;
};

struct EditorUIFrame final
{
	std::vector<EditorViewportRegion> Viewports;
	std::vector<EditorViewportTextureBinding> ViewportTextures;
	std::vector<pipeline::render::RenderViewID> CloseViewportRequests;
	std::vector<pipeline::render::RenderViewID> ToggleViewportProjectionRequests;
	uint32 CreateViewportRequestCount = 0;
	bool WantsKeyboard = false;
	bool WantsPointer = false;
	std::vector<EditorUIWindowFrame> Windows;
};

class EditorUserInterface final
{
  public:
	explicit EditorUserInterface(core::Window &Window);
	~EditorUserInterface();

	EditorUserInterface(const EditorUserInterface &) = delete;
	EditorUserInterface &operator=(const EditorUserInterface &) = delete;
	EditorUserInterface(EditorUserInterface &&) = delete;
	EditorUserInterface &operator=(EditorUserInterface &&) = delete;

	void InitializeRenderer();
	void ShutdownRenderer();
	void AttachWindowManager(core::WindowManager &Manager);
	void QueueInputEvent(const core::input::InputEvent &Event);
	[[nodiscard]] EditorUIFrame BuildFrame(const core::ApplicationFrame &Frame, EditorSession &Session,
										   action::EditorActionRegistry &Actions, action::EditorActionContext &ActionContext,
										   core::diagnostics::DiagnosticSink &Diagnostics,
										   std::span<const EditorViewportPresentation> Viewports, bool AllowWindowMutation);
	void Render(const EditorUIFrame &Frame, std::span<const EditorViewportPresentation> Viewports);
	void RecycleFrame(EditorUIFrame &&Frame) noexcept;

  private:
	struct State;
	std::unique_ptr<State> StateData;
};
} // namespace ui
} // namespace editor
