#pragma once

#include "Source/core/window/WindowTypes.h"

#include <imgui.h>

#include <span>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace core
{
class Window;
class WindowManager;
} // namespace core

namespace editor::ui
{
enum class EditorMouseCursorStyle : uint8
{
	Move,
	Rotate,
	Scale
};

class EditorUIWindowBridge final
{
  public:
	EditorUIWindowBridge(core::WindowManager &Manager, core::Window &MainWindow) noexcept;
	~EditorUIWindowBridge();

	EditorUIWindowBridge(const EditorUIWindowBridge &) = delete;
	EditorUIWindowBridge &operator=(const EditorUIWindowBridge &) = delete;
	EditorUIWindowBridge(EditorUIWindowBridge &&) = delete;
	EditorUIWindowBridge &operator=(EditorUIWindowBridge &&) = delete;

	void Install();
	void Shutdown() noexcept;
	void ProcessEvents(std::span<const core::WindowEvent> Events);
	void UpdateMouseCursor(std::optional<EditorMouseCursorStyle> Override = std::nullopt) noexcept;
	void UpdateWindows();
	void UpdateMonitors();
	void PrepareDetachedWindowTransfers();
	[[nodiscard]] core::Window *GetManagedWindow(const ImGuiViewport &Viewport) const noexcept;
	[[nodiscard]] core::Window *GetManagedWindow(core::WindowID Window) const noexcept;
	[[nodiscard]] bool IsInstalled() const noexcept;
	[[nodiscard]] std::optional<string> TakeCallbackDiagnostic();

  private:
	struct WindowData;

	[[nodiscard]] static EditorUIWindowBridge &Current();
	[[nodiscard]] static WindowData &RequireData(ImGuiViewport &Viewport);
	static void CreateManagedWindow(ImGuiViewport *Viewport);
	static void DestroyWindow(ImGuiViewport *Viewport);
	static void ShowWindow(ImGuiViewport *Viewport);
	static void SetWindowPosition(ImGuiViewport *Viewport, ImVec2 Position);
	[[nodiscard]] static ImVec2 GetWindowPosition(ImGuiViewport *Viewport);
	static void SetWindowSize(ImGuiViewport *Viewport, ImVec2 Size);
	[[nodiscard]] static ImVec2 GetWindowSize(ImGuiViewport *Viewport);
	[[nodiscard]] static ImVec2 GetFramebufferScale(ImGuiViewport *Viewport);
	static void SetWindowFocus(ImGuiViewport *Viewport);
	[[nodiscard]] static bool GetWindowFocus(ImGuiViewport *Viewport);
	[[nodiscard]] static bool GetWindowMinimized(ImGuiViewport *Viewport);
	static void SetWindowTitle(ImGuiViewport *Viewport, const char *Title);
	static void SetWindowAlpha(ImGuiViewport *Viewport, float Alpha);
	[[nodiscard]] static float GetWindowDpiScale(ImGuiViewport *Viewport);
	[[nodiscard]] static const char *GetClipboardText(void *UserData);
	static void SetClipboardText(void *UserData, const char *Text);

	core::WindowManager *Manager = nullptr;
	core::Window *MainWindow = nullptr;
	std::vector<core::MonitorInfo> MonitorsScratch;
	std::unordered_map<core::WindowID, int32> AppliedMouseCursors;
	string ClipboardTextScratch;
	std::optional<string> CallbackDiagnostic;
	bool Installed = false;
};
} // namespace editor::ui
