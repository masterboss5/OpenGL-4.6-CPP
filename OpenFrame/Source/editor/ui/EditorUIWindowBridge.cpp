#include "EditorUIWindowBridge.h"

#include "Source/core/window/Context.h"
#include "Source/core/window/Window.h"
#include "Source/core/window/WindowManager.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace editor::ui
{
struct EditorUIWindowBridge::WindowData final
{
	core::WindowID Window;
	bool Owned = false;
};

EditorUIWindowBridge::EditorUIWindowBridge(core::WindowManager &Manager, core::Window &MainWindow) noexcept
	: Manager(&Manager), MainWindow(&MainWindow)
{
}

EditorUIWindowBridge::~EditorUIWindowBridge()
{
	this->Shutdown();
}

void EditorUIWindowBridge::Install()
{
	if (this->Installed)
		return;
	if (ImGui::GetIO().BackendPlatformUserData != nullptr)
		throw std::logic_error("Dear ImGui already has an installed window bridge");
	this->Installed = true;
	try
	{

		ImGuiIO &IO = ImGui::GetIO();
		ImGuiPlatformIO &WindowIO = ImGui::GetPlatformIO();
		IO.BackendPlatformName = "EngineWindow";
		IO.BackendPlatformUserData = this;
		IO.GetClipboardTextFn = GetClipboardText;
		IO.SetClipboardTextFn = SetClipboardText;
		IO.ClipboardUserData = this;
		IO.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_HasMouseHoveredViewport;
		IO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		IO.ConfigDpiScaleFonts = true;
		IO.ConfigDpiScaleViewports = true;

		WindowIO.Platform_CreateWindow = CreateManagedWindow;
		WindowIO.Platform_DestroyWindow = DestroyWindow;
		WindowIO.Platform_ShowWindow = ShowWindow;
		WindowIO.Platform_SetWindowPos = SetWindowPosition;
		WindowIO.Platform_GetWindowPos = GetWindowPosition;
		WindowIO.Platform_SetWindowSize = SetWindowSize;
		WindowIO.Platform_GetWindowSize = GetWindowSize;
		WindowIO.Platform_GetWindowFramebufferScale = GetFramebufferScale;
		WindowIO.Platform_SetWindowFocus = SetWindowFocus;
		WindowIO.Platform_GetWindowFocus = GetWindowFocus;
		WindowIO.Platform_GetWindowMinimized = GetWindowMinimized;
		WindowIO.Platform_SetWindowTitle = SetWindowTitle;
		WindowIO.Platform_SetWindowAlpha = SetWindowAlpha;
		WindowIO.Platform_GetWindowDpiScale = GetWindowDpiScale;

		ImGuiViewport *MainViewport = ImGui::GetMainViewport();
		MainViewport->PlatformUserData = new WindowData{.Window = this->MainWindow->GetID(), .Owned = false};
		MainViewport->PlatformHandle = this->MainWindow;
		MainViewport->PlatformHandleRaw = this->MainWindow->GetSystemHandle();
		this->UpdateMonitors();
	}
	catch (...)
	{
		this->Shutdown();
		throw;
	}
}

void EditorUIWindowBridge::Shutdown() noexcept
{
	if (!this->Installed)
		return;
	try
	{
		ImGui::DestroyPlatformWindows();
		ImGuiViewport *MainViewport = ImGui::GetMainViewport();
		delete static_cast<WindowData *>(MainViewport->PlatformUserData);
		MainViewport->PlatformUserData = nullptr;
		MainViewport->PlatformHandle = nullptr;
		MainViewport->PlatformHandleRaw = nullptr;
		ImGuiPlatformIO &WindowIO = ImGui::GetPlatformIO();
		WindowIO.ClearPlatformHandlers();
		ImGuiIO &IO = ImGui::GetIO();
		IO.BackendFlags &= ~(ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_HasMouseHoveredViewport);
		IO.BackendPlatformName = nullptr;
		IO.BackendPlatformUserData = nullptr;
		IO.GetClipboardTextFn = nullptr;
		IO.SetClipboardTextFn = nullptr;
		IO.ClipboardUserData = nullptr;
	}
	catch (...)
	{
		std::terminate();
	}
	this->Installed = false;
}

void EditorUIWindowBridge::ProcessEvents(const std::span<const core::WindowEvent> Events)
{
	if (!this->Installed)
		return;
	for (const core::WindowEvent &Event : Events)
	{
		core::Window *Window = this->Manager->FindManagedWindow(Event.Window);
		if (Window == nullptr)
			continue;
		ImGuiViewport *Viewport = ImGui::FindViewportByPlatformHandle(Window);
		if (Viewport == nullptr)
			continue;
		switch (Event.Type)
		{
		case core::WindowEventType::CloseRequested:
			Viewport->PlatformRequestClose = true;
			Window->CancelClose();
			break;
		case core::WindowEventType::Moved:
			Viewport->PlatformRequestMove = true;
			break;
		case core::WindowEventType::Resized:
		case core::WindowEventType::FramebufferResized:
			Viewport->PlatformRequestResize = true;
			break;
		default:
			break;
		}
	}
}

void EditorUIWindowBridge::UpdateWindows()
{
	if (!this->Installed)
		throw std::logic_error("Dear ImGui window bridge is not installed");
	try
	{
		this->UpdateMonitors();
		ImGui::UpdatePlatformWindows();
	}
	catch (const std::exception &Exception)
	{
		this->CallbackDiagnostic = "Editor window callback failed: " + string(Exception.what());
	}
	catch (...)
	{
		this->CallbackDiagnostic = "Editor window callback failed with an unknown exception";
	}
}

void EditorUIWindowBridge::UpdateMonitors()
{
	ImGuiPlatformIO &WindowIO = ImGui::GetPlatformIO();
	WindowIO.Monitors.resize(0);
	this->Manager->GetMonitorsInto(this->MonitorsScratch);
	WindowIO.Monitors.reserve(static_cast<int>(this->MonitorsScratch.size()));
	for (const core::MonitorInfo &Monitor : this->MonitorsScratch)
	{
		ImGuiPlatformMonitor Destination;
		Destination.MainPos = ImVec2(static_cast<float32>(Monitor.Position.X), static_cast<float32>(Monitor.Position.Y));
		Destination.MainSize =
			ImVec2(static_cast<float32>(Monitor.CurrentMode.Extent.Width), static_cast<float32>(Monitor.CurrentMode.Extent.Height));
		Destination.WorkPos = ImVec2(static_cast<float32>(Monitor.WorkAreaPosition.X), static_cast<float32>(Monitor.WorkAreaPosition.Y));
		Destination.WorkSize =
			ImVec2(static_cast<float32>(Monitor.WorkAreaExtent.Width), static_cast<float32>(Monitor.WorkAreaExtent.Height));
		Destination.DpiScale = std::max(Monitor.ContentScaleX, Monitor.ContentScaleY);
		WindowIO.Monitors.push_back(Destination);
	}
}

void EditorUIWindowBridge::PrepareDetachedWindowTransfers()
{
	if (!this->Installed)
		return;
	const ImGuiViewport *MainViewport = ImGui::GetMainViewport();
	for (ImGuiViewport *Viewport : ImGui::GetPlatformIO().Viewports)
	{
		if (Viewport == MainViewport || Viewport == nullptr || Viewport->PlatformUserData == nullptr)
			continue;
		core::Window *Window = this->GetManagedWindow(*Viewport);
		if (Window == nullptr)
			continue;
		core::Context &Context = Window->GetContext();
		if (Context.IsThreadTransferPending())
			continue;
		Context.PrepareThreadTransfer();
	}
}

core::Window *EditorUIWindowBridge::GetManagedWindow(const ImGuiViewport &Viewport) const noexcept
{
	const auto *Data = static_cast<const WindowData *>(Viewport.PlatformUserData);
	return Data == nullptr ? nullptr : this->Manager->FindManagedWindow(Data->Window);
}

core::Window *EditorUIWindowBridge::GetManagedWindow(const core::WindowID Window) const noexcept
{
	return this->Manager->FindManagedWindow(Window);
}

bool EditorUIWindowBridge::IsInstalled() const noexcept
{
	return this->Installed;
}

std::optional<string> EditorUIWindowBridge::TakeCallbackDiagnostic()
{
	std::optional<string> Diagnostic = std::move(this->CallbackDiagnostic);
	this->CallbackDiagnostic.reset();
	return Diagnostic;
}

const char *EditorUIWindowBridge::GetClipboardText(void *UserData)
{
	auto *Bridge = static_cast<EditorUIWindowBridge *>(UserData);
	if (Bridge == nullptr || Bridge->MainWindow == nullptr)
		return "";
	try
	{
		const core::DataPayload Payload = Bridge->MainWindow->ReadClipboardData(
			{.Text = true, .Files = false, .Image = false, .AllNamedFormats = false, .MaximumPayloadBytes = 64U * 1024U * 1024U});
		Bridge->ClipboardTextScratch = Payload.Text.value_or(string{});
	}
	catch (...)
	{
		Bridge->ClipboardTextScratch.clear();
	}
	return Bridge->ClipboardTextScratch.c_str();
}

void EditorUIWindowBridge::SetClipboardText(void *UserData, const char *Text)
{
	auto *Bridge = static_cast<EditorUIWindowBridge *>(UserData);
	if (Bridge == nullptr || Bridge->MainWindow == nullptr || Text == nullptr)
		return;
	try
	{
		core::DataPayload Payload;
		Payload.Text = string(Text);
		Bridge->MainWindow->SetClipboardData(Payload);
	}
	catch (...)
	{
	}
}

EditorUIWindowBridge &EditorUIWindowBridge::Current()
{
	auto *Bridge = static_cast<EditorUIWindowBridge *>(ImGui::GetIO().BackendPlatformUserData);
	if (Bridge == nullptr)
		throw std::logic_error("Dear ImGui window callback has no owning bridge");
	return *Bridge;
}

EditorUIWindowBridge::WindowData &EditorUIWindowBridge::RequireData(ImGuiViewport &Viewport)
{
	auto *Data = static_cast<WindowData *>(Viewport.PlatformUserData);
	if (Data == nullptr)
		throw std::logic_error("Dear ImGui viewport has no managed window data");
	return *Data;
}

void EditorUIWindowBridge::CreateManagedWindow(ImGuiViewport *Viewport)
{
	if (Viewport == nullptr || Viewport->PlatformUserData != nullptr)
		throw std::invalid_argument("Dear ImGui requested an invalid managed window");
	EditorUIWindowBridge &Bridge = Current();
	const auto ToExtent = [](const float Value) { return static_cast<uint32>(std::max(1.0f, std::ceil(Value))); };
	core::WindowSpecification Specification;
	Specification.Title = "Editor";
	Specification.Extent = {ToExtent(Viewport->Size.x), ToExtent(Viewport->Size.y)};
	Specification.Position =
		core::WindowPosition{static_cast<int32>(std::lround(Viewport->Pos.x)), static_cast<int32>(std::lround(Viewport->Pos.y))};
	Specification.ContextGroup = Bridge.MainWindow->GetContext().GetShareGroupName();
	Specification.Visible = false;
	Specification.Focused = false;
	Specification.Decorated = (Viewport->Flags & ImGuiViewportFlags_NoDecoration) == 0;
	Specification.Resizable = Specification.Decorated;
	Specification.Floating = (Viewport->Flags & ImGuiViewportFlags_TopMost) != 0;
	core::Window &Window = Bridge.Manager->CreateWindow(Specification);
	Window.SetTaskbarVisible((Viewport->Flags & ImGuiViewportFlags_NoTaskBarIcon) == 0);
	if (Viewport->ParentViewport != nullptr)
	{
		if (core::Window *Owner = Bridge.GetManagedWindow(*Viewport->ParentViewport))
			Window.SetOwner(Owner);
	}
	(void)Window.SetDarkTheme(true);
	Window.GetContext().PrepareThreadTransfer();
	Viewport->PlatformUserData = new WindowData{.Window = Window.GetID(), .Owned = true};
	Viewport->PlatformHandle = &Window;
	Viewport->PlatformHandleRaw = Window.GetSystemHandle();
}

void EditorUIWindowBridge::DestroyWindow(ImGuiViewport *Viewport)
{
	if (Viewport == nullptr || Viewport->PlatformUserData == nullptr)
		return;
	EditorUIWindowBridge &Bridge = Current();
	WindowData *Data = static_cast<WindowData *>(Viewport->PlatformUserData);
	if (Data->Owned && Bridge.Manager->FindManagedWindow(Data->Window) != nullptr)
		Bridge.Manager->DestroyWindow(Data->Window);
	delete Data;
	Viewport->PlatformUserData = nullptr;
	Viewport->PlatformHandle = nullptr;
	Viewport->PlatformHandleRaw = nullptr;
}

void EditorUIWindowBridge::ShowWindow(ImGuiViewport *Viewport)
{
	Current().GetManagedWindow(*Viewport)->SetVisible(true);
}

void EditorUIWindowBridge::SetWindowPosition(ImGuiViewport *Viewport, const ImVec2 Position)
{
	Current().GetManagedWindow(*Viewport)->SetPosition(
		{static_cast<int32>(std::lround(Position.x)), static_cast<int32>(std::lround(Position.y))});
}

ImVec2 EditorUIWindowBridge::GetWindowPosition(ImGuiViewport *Viewport)
{
	const core::WindowPosition Position = Current().GetManagedWindow(*Viewport)->GetPosition();
	return {static_cast<float32>(Position.X), static_cast<float32>(Position.Y)};
}

void EditorUIWindowBridge::SetWindowSize(ImGuiViewport *Viewport, const ImVec2 Size)
{
	Current().GetManagedWindow(*Viewport)->SetExtent(
		{static_cast<uint32>(std::max(1.0f, std::ceil(Size.x))), static_cast<uint32>(std::max(1.0f, std::ceil(Size.y)))});
}

ImVec2 EditorUIWindowBridge::GetWindowSize(ImGuiViewport *Viewport)
{
	const core::WindowExtent Extent = Current().GetManagedWindow(*Viewport)->GetExtent();
	return {static_cast<float32>(Extent.Width), static_cast<float32>(Extent.Height)};
}

ImVec2 EditorUIWindowBridge::GetFramebufferScale(ImGuiViewport *Viewport)
{
	const core::Window *Window = Current().GetManagedWindow(*Viewport);
	const core::WindowExtent Extent = Window->GetExtent();
	const core::WindowExtent Framebuffer = Window->GetFramebufferExtent();
	return Extent.IsValid() ? ImVec2(static_cast<float32>(Framebuffer.Width) / static_cast<float32>(Extent.Width),
									 static_cast<float32>(Framebuffer.Height) / static_cast<float32>(Extent.Height))
							: ImVec2(1.0f, 1.0f);
}

void EditorUIWindowBridge::SetWindowFocus(ImGuiViewport *Viewport)
{
	Current().GetManagedWindow(*Viewport)->RequestFocus();
}

bool EditorUIWindowBridge::GetWindowFocus(ImGuiViewport *Viewport)
{
	return Current().GetManagedWindow(*Viewport)->IsFocused();
}

bool EditorUIWindowBridge::GetWindowMinimized(ImGuiViewport *Viewport)
{
	return Current().GetManagedWindow(*Viewport)->IsMinimized();
}

void EditorUIWindowBridge::SetWindowTitle(ImGuiViewport *Viewport, const char *Title)
{
	Current().GetManagedWindow(*Viewport)->SetTitle(Title == nullptr ? "Editor" : Title);
}

void EditorUIWindowBridge::SetWindowAlpha(ImGuiViewport *Viewport, const float Alpha)
{
	Current().GetManagedWindow(*Viewport)->SetOpacity(Alpha);
}

float EditorUIWindowBridge::GetWindowDpiScale(ImGuiViewport *Viewport)
{
	return Current().GetManagedWindow(*Viewport)->GetContentScale();
}
} // namespace editor::ui
