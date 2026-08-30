#include "EditorUIWindowBridge.h"

#include "Source/core/window/Context.h"
#include "Source/core/window/Window.h"
#include "Source/core/window/WindowManager.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace editor::ui
{
namespace
{
inline constexpr uint32 ToolCursorExtent = 32;
inline constexpr usize ToolCursorPixelBytes = static_cast<usize>(ToolCursorExtent) * ToolCursorExtent * 4U;
inline constexpr int32 ToolCursorKeyBase = 1'000;

struct ToolCursorImage final
{
	std::array<uint8, ToolCursorPixelBytes> Pixels{};
	core::WindowPosition HotSpot{16, 16};
};

void PutCursorPixel(ToolCursorImage &Image, const int32 X, const int32 Y, const std::array<uint8, 4> Color)
{
	if (X < 0 || Y < 0 || X >= static_cast<int32>(ToolCursorExtent) || Y >= static_cast<int32>(ToolCursorExtent))
		return;
	const usize Offset = (static_cast<usize>(Y) * ToolCursorExtent + static_cast<usize>(X)) * 4U;
	std::ranges::copy(Color, Image.Pixels.begin() + static_cast<std::ptrdiff_t>(Offset));
}

void DrawCursorLine(ToolCursorImage &Image, int32 X0, int32 Y0, const int32 X1, const int32 Y1, const int32 Radius,
					const std::array<uint8, 4> Color)
{
	const int32 DeltaX = std::abs(X1 - X0);
	const int32 StepX = X0 < X1 ? 1 : -1;
	const int32 DeltaY = -std::abs(Y1 - Y0);
	const int32 StepY = Y0 < Y1 ? 1 : -1;
	int32 Error = DeltaX + DeltaY;
	for (;;)
	{
		for (int32 OffsetY = -Radius; OffsetY <= Radius; ++OffsetY)
			for (int32 OffsetX = -Radius; OffsetX <= Radius; ++OffsetX)
				if (OffsetX * OffsetX + OffsetY * OffsetY <= Radius * Radius + 1)
					PutCursorPixel(Image, X0 + OffsetX, Y0 + OffsetY, Color);
		if (X0 == X1 && Y0 == Y1)
			break;
		const int32 TwiceError = Error * 2;
		if (TwiceError >= DeltaY)
		{
			Error += DeltaY;
			X0 += StepX;
		}
		if (TwiceError <= DeltaX)
		{
			Error += DeltaX;
			Y0 += StepY;
		}
	}
}

void DrawOutlinedCursorLine(ToolCursorImage &Image, const int32 X0, const int32 Y0, const int32 X1, const int32 Y1)
{
	static constexpr std::array<uint8, 4> Outline{11, 13, 16, 255};
	static constexpr std::array<uint8, 4> Foreground{241, 243, 247, 255};
	DrawCursorLine(Image, X0, Y0, X1, Y1, 2, Outline);
	DrawCursorLine(Image, X0, Y0, X1, Y1, 0, Foreground);
}

[[nodiscard]] ToolCursorImage BuildMoveCursor()
{
	ToolCursorImage Image;
	DrawOutlinedCursorLine(Image, 16, 5, 16, 27);
	DrawOutlinedCursorLine(Image, 5, 16, 27, 16);
	static constexpr std::array<std::array<int32, 4>, 8> ArrowHeads{
		std::array<int32, 4>{16, 5, 12, 9},	  std::array<int32, 4>{16, 5, 20, 9},  std::array<int32, 4>{16, 27, 12, 23},
		std::array<int32, 4>{16, 27, 20, 23}, std::array<int32, 4>{5, 16, 9, 12},  std::array<int32, 4>{5, 16, 9, 20},
		std::array<int32, 4>{27, 16, 23, 12}, std::array<int32, 4>{27, 16, 23, 20}};
	for (const std::array<int32, 4> Segment : ArrowHeads)
		DrawOutlinedCursorLine(Image, Segment[0], Segment[1], Segment[2], Segment[3]);
	return Image;
}

[[nodiscard]] ToolCursorImage BuildRotateCursor()
{
	ToolCursorImage Image;
	constexpr float32 Start = -2.55F;
	constexpr float32 End = 2.55F;
	constexpr int32 SegmentCount = 28;
	int32 PreviousX = 0;
	int32 PreviousY = 0;
	for (int32 Segment = 0; Segment <= SegmentCount; ++Segment)
	{
		const float32 Angle = Start + (End - Start) * static_cast<float32>(Segment) / static_cast<float32>(SegmentCount);
		const int32 X = static_cast<int32>(std::lround(16.0F + std::cos(Angle) * 10.0F));
		const int32 Y = static_cast<int32>(std::lround(16.0F + std::sin(Angle) * 10.0F));
		if (Segment != 0)
			DrawOutlinedCursorLine(Image, PreviousX, PreviousY, X, Y);
		PreviousX = X;
		PreviousY = Y;
	}
	DrawOutlinedCursorLine(Image, PreviousX, PreviousY, PreviousX - 1, PreviousY - 6);
	DrawOutlinedCursorLine(Image, PreviousX, PreviousY, PreviousX - 6, PreviousY - 1);
	return Image;
}

[[nodiscard]] ToolCursorImage BuildScaleCursor()
{
	ToolCursorImage Image;
	DrawOutlinedCursorLine(Image, 8, 24, 24, 8);
	DrawOutlinedCursorLine(Image, 24, 8, 17, 8);
	DrawOutlinedCursorLine(Image, 24, 8, 24, 15);
	DrawOutlinedCursorLine(Image, 8, 24, 15, 24);
	DrawOutlinedCursorLine(Image, 8, 24, 8, 17);
	return Image;
}

[[nodiscard]] const ToolCursorImage &GetToolCursor(const EditorMouseCursorStyle Style)
{
	static const ToolCursorImage Move = BuildMoveCursor();
	static const ToolCursorImage Rotate = BuildRotateCursor();
	static const ToolCursorImage Scale = BuildScaleCursor();
	switch (Style)
	{
	case EditorMouseCursorStyle::Move:
		return Move;
	case EditorMouseCursorStyle::Rotate:
		return Rotate;
	case EditorMouseCursorStyle::Scale:
		return Scale;
	}
	return Move;
}
} // namespace

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
		IO.BackendFlags |=
			ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_HasMouseHoveredViewport | ImGuiBackendFlags_HasMouseCursors;
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
		IO.BackendFlags &=
			~(ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_HasMouseHoveredViewport | ImGuiBackendFlags_HasMouseCursors);
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
	this->AppliedMouseCursors.clear();
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

void EditorUIWindowBridge::UpdateMouseCursor(const std::optional<EditorMouseCursorStyle> Override) noexcept
{
	if (!this->Installed || (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0)
		return;
	try
	{
		const ImGuiIO &IO = ImGui::GetIO();
		ImGuiViewport *HoveredViewport = ImGui::FindViewportByID(IO.MouseHoveredViewport);
		if (HoveredViewport == nullptr)
			return;
		core::Window *Window = this->GetManagedWindow(*HoveredViewport);
		if (Window == nullptr)
			return;
		const int32 CursorKey =
			Override.has_value() ? ToolCursorKeyBase + static_cast<int32>(*Override) : static_cast<int32>(ImGui::GetMouseCursor());
		if (const auto Applied = this->AppliedMouseCursors.find(Window->GetID());
			Applied != this->AppliedMouseCursors.end() && Applied->second == CursorKey)
			return;
		if (Override.has_value())
		{
			const ToolCursorImage &Cursor = GetToolCursor(*Override);
			Window->SetCustomCursor({.Pixels = Cursor.Pixels.data(), .Extent = {ToolCursorExtent, ToolCursorExtent}, .BytesPerPixel = 4},
									Cursor.HotSpot);
			this->AppliedMouseCursors[Window->GetID()] = CursorKey;
			return;
		}

		core::CursorShape Shape = core::CursorShape::Arrow;
		switch (ImGui::GetMouseCursor())
		{
		case ImGuiMouseCursor_TextInput:
			Shape = core::CursorShape::Text;
			break;
		case ImGuiMouseCursor_ResizeAll:
			Shape = core::CursorShape::ResizeAll;
			break;
		case ImGuiMouseCursor_ResizeNS:
			Shape = core::CursorShape::ResizeVertical;
			break;
		case ImGuiMouseCursor_ResizeEW:
			Shape = core::CursorShape::ResizeHorizontal;
			break;
		case ImGuiMouseCursor_ResizeNESW:
			Shape = core::CursorShape::ResizeNortheastSouthwest;
			break;
		case ImGuiMouseCursor_ResizeNWSE:
			Shape = core::CursorShape::ResizeNorthwestSoutheast;
			break;
		case ImGuiMouseCursor_Hand:
			Shape = core::CursorShape::PointingHand;
			break;
		case ImGuiMouseCursor_NotAllowed:
			Shape = core::CursorShape::NotAllowed;
			break;
		default:
			break;
		}
		Window->SetCursorShape(Shape);
		this->AppliedMouseCursors[Window->GetID()] = CursorKey;
	}
	catch (const std::exception &Exception)
	{
		this->CallbackDiagnostic = "Editor mouse cursor update failed: " + string(Exception.what());
	}
	catch (...)
	{
		this->CallbackDiagnostic = "Editor mouse cursor update failed with an unknown exception";
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
	Bridge.AppliedMouseCursors.erase(Data->Window);
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
