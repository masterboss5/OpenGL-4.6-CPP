#include "Window.h"
#include "Context.h"
#include "WindowException.h"
#include "WindowManager.h"
#include "src/core/input/InputTypes.h"

#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <Dbt.h>
#include <Imm.h>
#include <WtsApi32.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#ifdef IsMinimized
#undef IsMinimized
#endif
#ifdef IsMaximized
#undef IsMaximized
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace core
{
namespace
{
[[nodiscard]] std::wstring ToWide(const std::string_view Source)
{
	if (Source.empty())
		return {};
	const int32 Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), nullptr, 0);
	if (Length <= 0)
		throw WindowException("Text is not valid UTF-8");
	std::wstring Result(static_cast<usize>(Length), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), Result.data(), Length) !=
		Length)
		throw WindowException("UTF-8 conversion failed");
	return Result;
}

[[nodiscard]] HICON CreateIcon(const WindowImageView &Image)
{
	if (!Image.IsValid())
		throw WindowException("Icon requires a valid RGBA8 image");
	if (Image.Extent.Width > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
		Image.Extent.Height > static_cast<uint32>(std::numeric_limits<int32>::max()))
		throw WindowException("Icon dimensions exceed native limits");
	BITMAPV5HEADER Header{};
	Header.bV5Size = sizeof(Header);
	Header.bV5Width = static_cast<LONG>(Image.Extent.Width);
	Header.bV5Height = -static_cast<LONG>(Image.Extent.Height);
	Header.bV5Planes = 1;
	Header.bV5BitCount = 32;
	Header.bV5Compression = BI_BITFIELDS;
	Header.bV5RedMask = 0x00FF0000;
	Header.bV5GreenMask = 0x0000FF00;
	Header.bV5BlueMask = 0x000000FF;
	Header.bV5AlphaMask = 0xFF000000;
	void *BitmapPixels = nullptr;
	HDC Screen = GetDC(nullptr);
	HBITMAP Color = CreateDIBSection(Screen, reinterpret_cast<const BITMAPINFO *>(&Header), DIB_RGB_COLORS, &BitmapPixels, nullptr, 0);
	ReleaseDC(nullptr, Screen);
	if (Color == nullptr || BitmapPixels == nullptr)
		throw WindowException("Failed to create icon color bitmap");
	const usize PixelCount = static_cast<usize>(Image.Extent.Width) * Image.Extent.Height;
	uint8 *Destination = static_cast<uint8 *>(BitmapPixels);
	for (usize Pixel = 0; Pixel < PixelCount; ++Pixel)
	{
		Destination[Pixel * 4 + 0] = Image.Pixels[Pixel * 4 + 2];
		Destination[Pixel * 4 + 1] = Image.Pixels[Pixel * 4 + 1];
		Destination[Pixel * 4 + 2] = Image.Pixels[Pixel * 4 + 0];
		Destination[Pixel * 4 + 3] = Image.Pixels[Pixel * 4 + 3];
	}
	HBITMAP Mask = CreateBitmap(static_cast<int32>(Image.Extent.Width), static_cast<int32>(Image.Extent.Height), 1, 1, nullptr);
	if (Mask == nullptr)
	{
		DeleteObject(Color);
		throw WindowException("Failed to create icon mask bitmap");
	}
	ICONINFO IconInfo{.fIcon = TRUE, .hbmMask = Mask, .hbmColor = Color};
	HICON Icon = CreateIconIndirect(&IconInfo);
	DeleteObject(Mask);
	DeleteObject(Color);
	if (Icon == nullptr)
		throw WindowException("Failed to create native icon");
	return Icon;
}
} // namespace

struct Window::State final
{
	WindowManager *Manager = nullptr;
	Context *Context = nullptr;
	GLFWwindow *NativeWindow = nullptr;
	WindowID ID;
	std::string Title;
	WindowExtent Extent;
	WindowExtent FramebufferExtent;
	WindowPosition Position;
	WindowPosition WindowedPosition;
	WindowExtent WindowedExtent;
	WindowSizeConstraints Constraints;
	WindowMode Mode = WindowMode::Windowed;
	PresentationMode PresentationMode = PresentationMode::Off;
	float32 ContentScale = 1.0f;
	bool Visible = false;
	bool Focused = false;
	bool Minimized = false;
	bool Maximized = false;
	bool SRGBPresentationCapable = false;
	bool NativeEventsAttached = false;
	void *Monitor = nullptr;
	uint64 LastGestureArgument = 0;
	WindowPosition LastGesturePosition;
	float32 LastGestureRotation = 0.0f;
	bool GestureActive = false;
	GLFWcursor *Cursor = nullptr;
	CursorMode CursorMode = CursorMode::Visible;
	WindowID Owner;
	bool Modal = false;
	TitleBarSpecification TitleBar;
	WindowHitTest HitTest;
	GLFWmonitor *TargetMonitor = nullptr;
	std::optional<MonitorVideoMode> RequestedVideoMode;
	ITaskbarList3 *Taskbar = nullptr;
	std::vector<HICON> ThumbnailIcons;
	std::array<uint16, 7> ThumbnailCommands{};
	bool ThumbnailButtonsAdded = false;
	bool NotificationIconRegistered = false;
	void *DataDropTarget = nullptr;
	std::vector<std::shared_ptr<detail::DialogOperationBase>> DialogOperations;
};

struct WindowCallbacks final
{
	[[nodiscard]] static LRESULT ToNativeHitTest(const WindowHitRegion Region) noexcept
	{
		switch (Region)
		{
		case WindowHitRegion::Caption:
			return HTCAPTION;
		case WindowHitRegion::SystemMenu:
			return HTSYSMENU;
		case WindowHitRegion::Minimize:
			return HTMINBUTTON;
		case WindowHitRegion::Maximize:
			return HTMAXBUTTON;
		case WindowHitRegion::Close:
			return HTCLOSE;
		case WindowHitRegion::ResizeLeft:
			return HTLEFT;
		case WindowHitRegion::ResizeRight:
			return HTRIGHT;
		case WindowHitRegion::ResizeTop:
			return HTTOP;
		case WindowHitRegion::ResizeBottom:
			return HTBOTTOM;
		case WindowHitRegion::ResizeTopLeft:
			return HTTOPLEFT;
		case WindowHitRegion::ResizeTopRight:
			return HTTOPRIGHT;
		case WindowHitRegion::ResizeBottomLeft:
			return HTBOTTOMLEFT;
		case WindowHitRegion::ResizeBottomRight:
			return HTBOTTOMRIGHT;
		default:
			return HTCLIENT;
		}
	}
	[[nodiscard]] static std::u32string ToUtf32(const std::wstring_view Source)
	{
		std::u32string Result;
		Result.reserve(Source.size());
		for (usize Index = 0; Index < Source.size(); ++Index)
		{
			const uint32 First = static_cast<uint16>(Source[Index]);
			if (First >= 0xD800U && First <= 0xDBFFU && Index + 1 < Source.size())
			{
				const uint32 Second = static_cast<uint16>(Source[Index + 1]);
				if (Second >= 0xDC00U && Second <= 0xDFFFU)
				{
					Result.push_back(static_cast<char32_t>(0x10000U + ((First - 0xD800U) << 10U) + (Second - 0xDC00U)));
					++Index;
					continue;
				}
			}
			if (First < 0xD800U || First > 0xDFFFU)
				Result.push_back(static_cast<char32_t>(First));
		}
		return Result;
	}

	[[nodiscard]] static input::ContactPhase ToContactPhase(const UINT Message) noexcept
	{
		if (Message == WM_POINTERDOWN)
			return input::ContactPhase::Began;
		if (Message == WM_POINTERUP)
			return input::ContactPhase::Ended;
		if (Message == WM_POINTERCAPTURECHANGED)
			return input::ContactPhase::Cancelled;
		return input::ContactPhase::Moved;
	}

	static LRESULT CALLBACK NativeProcedure(HWND NativeWindow, UINT Message, WPARAM WParam, LPARAM LParam, UINT_PTR,
											DWORD_PTR ReferenceData)
	{
		Window *WindowInstance = reinterpret_cast<Window *>(ReferenceData);
		if (WindowInstance == nullptr)
			return DefSubclassProc(NativeWindow, Message, WParam, LParam);
		try
		{
			if (Message == WM_NCCALCSIZE && WindowInstance->StateData->TitleBar.Custom)
				return 0;
			if (Message == WM_COMMAND && HIWORD(WParam) == THBN_CLICKED)
			{
				const uint16 NativeCommand = LOWORD(WParam);
				if (NativeCommand >= 1 && NativeCommand <= WindowInstance->StateData->ThumbnailCommands.size())
				{
					const uint16 Command = WindowInstance->StateData->ThumbnailCommands[NativeCommand - 1];
					if (Command != 0)
						WindowInstance->Publish(
							{.Type = WindowEventType::ThumbnailActionInvoked, .Window = WindowInstance->GetID(), .CommandID = Command});
				}
				return 0;
			}
			if (Message == WM_NCHITTEST && WindowInstance->StateData->TitleBar.Custom)
			{
				const LRESULT SystemResult = DefSubclassProc(NativeWindow, Message, WParam, LParam);
				if (WindowInstance->StateData->TitleBar.PreserveResizeBorder && SystemResult != HTCLIENT)
					return SystemResult;
				POINT Point{GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam)};
				ScreenToClient(NativeWindow, &Point);
				if (WindowInstance->StateData->HitTest)
				{
					try
					{
						return ToNativeHitTest(WindowInstance->StateData->HitTest({Point.x, Point.y}));
					}
					catch (...)
					{
						WindowInstance->StateData->Manager->RecordNativeException(std::current_exception());
						return HTCLIENT;
					}
				}
				return Point.y >= 0 && Point.y < static_cast<int32>(WindowInstance->StateData->TitleBar.DraggableHeight) ? HTCAPTION
																														 : HTCLIENT;
			}
			if (Message == WM_DPICHANGED)
			{
				const RECT *Suggested = reinterpret_cast<const RECT *>(LParam);
				if (Suggested != nullptr)
					SetWindowPos(NativeWindow, nullptr, Suggested->left, Suggested->top, Suggested->right - Suggested->left,
								 Suggested->bottom - Suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
				WindowInstance->Publish({.Type = WindowEventType::DPIChanged,
										 .Window = WindowInstance->GetID(),
										 .ContentScaleX = static_cast<float32>(LOWORD(WParam)) / 96.0f,
										 .ContentScaleY = static_cast<float32>(HIWORD(WParam)) / 96.0f});
			}
			else if (Message == WM_DISPLAYCHANGE)
				WindowInstance->Publish({.Type = WindowEventType::DisplayChanged, .Window = WindowInstance->GetID()});
			else if (Message == WM_THEMECHANGED)
				WindowInstance->Publish({.Type = WindowEventType::ThemeChanged, .Window = WindowInstance->GetID()});
			else if (Message == WM_POWERBROADCAST)
				WindowInstance->Publish(
					{.Type = WindowEventType::PowerChanged, .Window = WindowInstance->GetID(), .State = WParam != PBT_APMSUSPEND});
			else if (Message == WM_WTSSESSION_CHANGE)
				WindowInstance->Publish({.Type = WindowEventType::SessionChanged,
										 .Window = WindowInstance->GetID(),
										 .State = WParam == WTS_SESSION_UNLOCK || WParam == WTS_SESSION_LOGON});
			else if (Message == WM_DEVICECHANGE)
				WindowInstance->Publish(
					{.Type = WindowEventType::DeviceChanged, .Window = WindowInstance->GetID(), .State = WParam == DBT_DEVICEARRIVAL});
			else if (Message == WM_COMPACTING)
				WindowInstance->Publish({.Type = WindowEventType::MemoryPressure, .Window = WindowInstance->GetID()});
			else if (Message == WM_SETTINGCHANGE)
			{
				const wchar_t *Setting = reinterpret_cast<const wchar_t *>(LParam);
				if (Setting != nullptr && std::wstring_view(Setting) == L"ImmersiveColorSet")
					WindowInstance->Publish({.Type = WindowEventType::ThemeChanged, .Window = WindowInstance->GetID()});
				WindowInstance->Publish({.Type = WindowEventType::AccessibilityChanged, .Window = WindowInstance->GetID()});
			}
			else if (Message == WM_WINDOWPOSCHANGED)
			{
				HMONITOR Monitor = MonitorFromWindow(NativeWindow, MONITOR_DEFAULTTONEAREST);
				if (WindowInstance->StateData->Monitor != Monitor)
				{
					WindowInstance->StateData->Monitor = Monitor;
					WindowInstance->Publish({.Type = WindowEventType::MonitorChanged, .Window = WindowInstance->GetID()});
				}
				if (WindowInstance->StateData->CursorMode == CursorMode::Confined && WindowInstance->StateData->Focused)
				{
					RECT ClientRectangle{};
					POINT TopLeft{};
					POINT BottomRight{};
					if (GetClientRect(NativeWindow, &ClientRectangle) == FALSE)
					{
						WindowInstance->StateData->Manager->RecordNativeException(
							std::make_exception_ptr(WindowException("Failed to update cursor confinement after window movement")));
					}
					else
					{
						TopLeft = {ClientRectangle.left, ClientRectangle.top};
						BottomRight = {ClientRectangle.right, ClientRectangle.bottom};
						RECT Confinement{TopLeft.x, TopLeft.y, BottomRight.x, BottomRight.y};
						if (ClientToScreen(NativeWindow, &TopLeft) == FALSE || ClientToScreen(NativeWindow, &BottomRight) == FALSE)
						{
							WindowInstance->StateData->Manager->RecordNativeException(
								std::make_exception_ptr(WindowException("Failed to update cursor confinement after window movement")));
						}
						else
						{
							Confinement = {TopLeft.x, TopLeft.y, BottomRight.x, BottomRight.y};
							if (ClipCursor(&Confinement) == FALSE)
								WindowInstance->StateData->Manager->RecordNativeException(
									std::make_exception_ptr(WindowException("Failed to update cursor confinement after window movement")));
						}
					}
				}
			}
			else if (Message == WM_IME_COMPOSITION || Message == WM_IME_NOTIFY)
			{
				HIMC InputContext = ImmGetContext(NativeWindow);
				if (InputContext != nullptr)
				{
					input::InputEvent Event{.Type = input::InputEventType::IMEComposition, .Window = WindowInstance->GetID()};
					if (Message == WM_IME_COMPOSITION && (LParam & GCS_COMPSTR) != 0)
					{
						const LONG ByteCount = ImmGetCompositionStringW(InputContext, GCS_COMPSTR, nullptr, 0);
						if (ByteCount > 0)
						{
							std::wstring Composition(static_cast<usize>(ByteCount) / sizeof(wchar_t), L'\0');
							(void)ImmGetCompositionStringW(InputContext, GCS_COMPSTR, Composition.data(), static_cast<DWORD>(ByteCount));
							Event.Composition = ToUtf32(Composition);
						}
					}
					const LONG Cursor = ImmGetCompositionStringW(InputContext, GCS_CURSORPOS, nullptr, 0);
					Event.CompositionCursor = Cursor < 0 ? 0U : static_cast<uint32>(Cursor);
					const DWORD CandidateBytes = ImmGetCandidateListW(InputContext, 0, nullptr, 0);
					if (CandidateBytes >= sizeof(CANDIDATELIST))
					{
						std::vector<uint8> CandidateStorage(CandidateBytes);
						auto *Candidates = reinterpret_cast<CANDIDATELIST *>(CandidateStorage.data());
						if (ImmGetCandidateListW(InputContext, 0, Candidates, CandidateBytes) != 0)
						{
							Event.SelectedCompositionCandidate = Candidates->dwSelection;
							Event.CompositionCandidates.reserve(Candidates->dwCount);
							for (DWORD Index = 0; Index < Candidates->dwCount; ++Index)
							{
								const DWORD Offset = Candidates->dwOffset[Index];
								if (Offset >= CandidateBytes)
									continue;
								const wchar_t *Candidate = reinterpret_cast<const wchar_t *>(CandidateStorage.data() + Offset);
								const usize RemainingCharacters = (CandidateBytes - Offset) / sizeof(wchar_t);
								const wchar_t *Terminator = std::find(Candidate, Candidate + RemainingCharacters, L'\0');
								if (Terminator != Candidate + RemainingCharacters)
									Event.CompositionCandidates.push_back(ToUtf32(std::wstring_view(Candidate, Terminator)));
							}
						}
					}
					ImmReleaseContext(NativeWindow, InputContext);
					WindowInstance->StateData->Manager->EnqueueInputEvent(std::move(Event));
				}
			}
			else if (Message == WM_IME_ENDCOMPOSITION)
			{
				WindowInstance->StateData->Manager->EnqueueInputEvent(
					{.Type = input::InputEventType::IMEComposition, .Window = WindowInstance->GetID()});
			}
			else if (Message == WM_POINTERDOWN || Message == WM_POINTERUPDATE || Message == WM_POINTERUP ||
					 Message == WM_POINTERCAPTURECHANGED)
			{
				const uint32 PointerID = GET_POINTERID_WPARAM(WParam);
				POINTER_INPUT_TYPE PointerType = PT_POINTER;
				if (GetPointerType(PointerID, &PointerType) != FALSE && PointerType == PT_TOUCH)
				{
					POINTER_TOUCH_INFO Info{};
					if (GetPointerTouchInfo(PointerID, &Info) != FALSE)
					{
						POINT Position = Info.pointerInfo.ptPixelLocation;
						ScreenToClient(NativeWindow, &Position);
						const RECT Contact = Info.rcContact;
						WindowInstance->StateData->Manager->EnqueueInputEvent(
							{.Type = input::InputEventType::Touch,
							 .Window = WindowInstance->GetID(),
							 .Touch = {.ID = PointerID,
									   .Phase = ToContactPhase(Message),
									   .X = static_cast<float64>(Position.x),
									   .Y = static_cast<float64>(Position.y),
									   .Pressure = (Info.touchMask & TOUCH_MASK_PRESSURE) != 0
													   ? static_cast<float32>(Info.pressure) / 1024.0f
													   : 1.0f,
									   .ContactExtent = {static_cast<uint32>(std::max<LONG>(Contact.right - Contact.left, 0)),
														 static_cast<uint32>(std::max<LONG>(Contact.bottom - Contact.top, 0))},
									   .Primary = (Info.pointerInfo.pointerFlags & POINTER_FLAG_PRIMARY) != 0}});
					}
				}
				else if (PointerType == PT_PEN)
				{
					POINTER_PEN_INFO Info{};
					if (GetPointerPenInfo(PointerID, &Info) != FALSE)
					{
						POINT Position = Info.pointerInfo.ptPixelLocation;
						ScreenToClient(NativeWindow, &Position);
						WindowInstance->StateData->Manager->EnqueueInputEvent(
							{.Type = input::InputEventType::Pen,
							 .Window = WindowInstance->GetID(),
							 .Pen = {.ID = PointerID,
									 .Phase = ToContactPhase(Message),
									 .X = static_cast<float64>(Position.x),
									 .Y = static_cast<float64>(Position.y),
									 .Pressure =
										 (Info.penMask & PEN_MASK_PRESSURE) != 0 ? static_cast<float32>(Info.pressure) / 1024.0f : 0.0f,
									 .TiltX = (Info.penMask & PEN_MASK_TILT_X) != 0 ? static_cast<float32>(Info.tiltX) : 0.0f,
									 .TiltY = (Info.penMask & PEN_MASK_TILT_Y) != 0 ? static_cast<float32>(Info.tiltY) : 0.0f,
									 .Rotation = (Info.penMask & PEN_MASK_ROTATION) != 0 ? static_cast<float32>(Info.rotation) : 0.0f,
									 .Eraser = (Info.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0,
									 .Primary = (Info.pointerInfo.pointerFlags & POINTER_FLAG_PRIMARY) != 0}});
					}
				}
			}
			else if (Message == WM_GESTURE)
			{
				GESTUREINFO Info{.cbSize = sizeof(GESTUREINFO)};
				if (GetGestureInfo(reinterpret_cast<HGESTUREINFO>(LParam), &Info) != FALSE)
				{
					CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(LParam));
					POINT Position{Info.ptsLocation.x, Info.ptsLocation.y};
					ScreenToClient(NativeWindow, &Position);
					input::Gesture Gesture{.X = static_cast<float64>(Position.x), .Y = static_cast<float64>(Position.y)};
					bool Emit = true;
					if (Info.dwID == GID_BEGIN)
					{
						WindowInstance->StateData->GestureActive = false;
						Emit = false;
					}
					else if (Info.dwID == GID_END)
					{
						WindowInstance->StateData->GestureActive = false;
						Emit = false;
					}
					else if (Info.dwID == GID_PAN)
					{
						Gesture.Type = input::GestureType::Pan;
						if (WindowInstance->StateData->GestureActive)
						{
							Gesture.DeltaX = Position.x - WindowInstance->StateData->LastGesturePosition.X;
							Gesture.DeltaY = Position.y - WindowInstance->StateData->LastGesturePosition.Y;
						}
					}
					else if (Info.dwID == GID_ZOOM)
					{
						Gesture.Type = input::GestureType::Zoom;
						Gesture.Value = WindowInstance->StateData->GestureActive && WindowInstance->StateData->LastGestureArgument != 0
											? static_cast<float32>(Info.ullArguments) /
												  static_cast<float32>(WindowInstance->StateData->LastGestureArgument)
											: 1.0f;
					}
					else if (Info.dwID == GID_ROTATE)
					{
						Gesture.Type = input::GestureType::Rotate;
						const float32 Rotation = static_cast<float32>(GID_ROTATE_ANGLE_FROM_ARGUMENT(Info.ullArguments));
						Gesture.Value =
							WindowInstance->StateData->GestureActive ? Rotation - WindowInstance->StateData->LastGestureRotation : 0.0f;
						WindowInstance->StateData->LastGestureRotation = Rotation;
					}
					else if (Info.dwID == GID_TWOFINGERTAP)
						Gesture.Type = input::GestureType::TwoFingerTap;
					else if (Info.dwID == GID_PRESSANDTAP)
						Gesture.Type = input::GestureType::PressAndTap;
					else
						Emit = false;
					WindowInstance->StateData->LastGestureArgument = Info.ullArguments;
					WindowInstance->StateData->LastGesturePosition = {Position.x, Position.y};
					WindowInstance->StateData->GestureActive = true;
					if (Emit)
						WindowInstance->StateData->Manager->EnqueueInputEvent(
							{.Type = input::InputEventType::Gesture, .Window = WindowInstance->GetID(), .Gesture = Gesture});
					return 0;
				}
			}
			return DefSubclassProc(NativeWindow, Message, WParam, LParam);
		}
		catch (...)
		{
			WindowInstance->RecordNativeFailure(std::current_exception());
			return Message == WM_GESTURE ? 0 : DefSubclassProc(NativeWindow, Message, WParam, LParam);
		}
	}

	[[nodiscard]] static input::Key ToKey(const int32 Key) noexcept
	{
		using enum input::Key;
		if (Key >= GLFW_KEY_A && Key <= GLFW_KEY_Z)
			return static_cast<input::Key>(static_cast<uint16>(A) + static_cast<uint16>(Key - GLFW_KEY_A));
		if (Key >= GLFW_KEY_0 && Key <= GLFW_KEY_9)
			return static_cast<input::Key>(static_cast<uint16>(Number0) + static_cast<uint16>(Key - GLFW_KEY_0));
		switch (Key)
		{
		case GLFW_KEY_SPACE:
			return Space;
		case GLFW_KEY_APOSTROPHE:
			return Apostrophe;
		case GLFW_KEY_COMMA:
			return Comma;
		case GLFW_KEY_MINUS:
			return Minus;
		case GLFW_KEY_PERIOD:
			return Period;
		case GLFW_KEY_SLASH:
			return Slash;
		case GLFW_KEY_SEMICOLON:
			return Semicolon;
		case GLFW_KEY_EQUAL:
			return Equal;
		case GLFW_KEY_ESCAPE:
			return Escape;
		case GLFW_KEY_ENTER:
			return Enter;
		case GLFW_KEY_TAB:
			return Tab;
		case GLFW_KEY_BACKSPACE:
			return Backspace;
		case GLFW_KEY_INSERT:
			return Insert;
		case GLFW_KEY_DELETE:
			return Delete;
		case GLFW_KEY_RIGHT:
			return Right;
		case GLFW_KEY_LEFT:
			return Left;
		case GLFW_KEY_DOWN:
			return Down;
		case GLFW_KEY_UP:
			return Up;
		case GLFW_KEY_PAGE_UP:
			return PageUp;
		case GLFW_KEY_PAGE_DOWN:
			return PageDown;
		case GLFW_KEY_HOME:
			return Home;
		case GLFW_KEY_END:
			return End;
		case GLFW_KEY_CAPS_LOCK:
			return CapsLock;
		case GLFW_KEY_SCROLL_LOCK:
			return ScrollLock;
		case GLFW_KEY_NUM_LOCK:
			return NumLock;
		case GLFW_KEY_PRINT_SCREEN:
			return PrintScreen;
		case GLFW_KEY_PAUSE:
			return Pause;
		case GLFW_KEY_F1:
			return F1;
		case GLFW_KEY_F2:
			return F2;
		case GLFW_KEY_F3:
			return F3;
		case GLFW_KEY_F4:
			return F4;
		case GLFW_KEY_F5:
			return F5;
		case GLFW_KEY_F6:
			return F6;
		case GLFW_KEY_F7:
			return F7;
		case GLFW_KEY_F8:
			return F8;
		case GLFW_KEY_F9:
			return F9;
		case GLFW_KEY_F10:
			return F10;
		case GLFW_KEY_F11:
			return F11;
		case GLFW_KEY_F12:
			return F12;
		case GLFW_KEY_LEFT_SHIFT:
			return LeftShift;
		case GLFW_KEY_LEFT_CONTROL:
			return LeftControl;
		case GLFW_KEY_LEFT_ALT:
			return LeftAlt;
		case GLFW_KEY_LEFT_SUPER:
			return LeftSuper;
		case GLFW_KEY_RIGHT_SHIFT:
			return RightShift;
		case GLFW_KEY_RIGHT_CONTROL:
			return RightControl;
		case GLFW_KEY_RIGHT_ALT:
			return RightAlt;
		case GLFW_KEY_RIGHT_SUPER:
			return RightSuper;
		case GLFW_KEY_MENU:
			return Menu;
		default:
			return Unknown;
		}
	}

	[[nodiscard]] static input::Modifier ToModifiers(const int32 Modifiers) noexcept
	{
		input::Modifier Result = input::Modifier::None;
		if ((Modifiers & GLFW_MOD_SHIFT) != 0)
			Result = Result | input::Modifier::Shift;
		if ((Modifiers & GLFW_MOD_CONTROL) != 0)
			Result = Result | input::Modifier::Control;
		if ((Modifiers & GLFW_MOD_ALT) != 0)
			Result = Result | input::Modifier::Alt;
		if ((Modifiers & GLFW_MOD_SUPER) != 0)
			Result = Result | input::Modifier::Super;
		if ((Modifiers & GLFW_MOD_CAPS_LOCK) != 0)
			Result = Result | input::Modifier::CapsLock;
		if ((Modifiers & GLFW_MOD_NUM_LOCK) != 0)
			Result = Result | input::Modifier::NumLock;
		return Result;
	}

	[[nodiscard]] static input::MouseButton ToMouseButton(const int32 Button) noexcept
	{
		return Button >= GLFW_MOUSE_BUTTON_1 && Button <= GLFW_MOUSE_BUTTON_8
				   ? static_cast<input::MouseButton>(Button - GLFW_MOUSE_BUTTON_1)
				   : input::MouseButton::Left;
	}

	static Window *Get(GLFWwindow *NativeWindow)
	{
		return static_cast<Window *>(glfwGetWindowUserPointer(NativeWindow));
	}
	template <typename Callback> static void Invoke(GLFWwindow *NativeWindow, Callback &&Function) noexcept
	{
		Window *WindowInstance = Get(NativeWindow);
		if (WindowInstance == nullptr)
			return;
		try
		{
			Function(*WindowInstance);
		}
		catch (...)
		{
			WindowInstance->RecordNativeFailure(std::current_exception());
		}
	}

	static void Close(GLFWwindow *NativeWindow) noexcept
	{
		Invoke(NativeWindow, [](Window &WindowInstance)
			   { WindowInstance.Publish({.Type = WindowEventType::CloseRequested, .Window = WindowInstance.GetID()}); });
	}

	static void Position(GLFWwindow *NativeWindow, int32 X, int32 Y) noexcept
	{
		Invoke(NativeWindow,
			   [X, Y](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Position = {X, Y};
				   WindowInstance.Publish({.Type = WindowEventType::Moved, .Window = WindowInstance.GetID(), .Position = {X, Y}});
			   });
	}

	static void Size(GLFWwindow *NativeWindow, int32 Width, int32 Height) noexcept
	{
		if (Width < 0 || Height < 0)
			return;
		Invoke(NativeWindow,
			   [Width, Height](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Extent = {static_cast<uint32>(Width), static_cast<uint32>(Height)};
				   WindowInstance.Publish(
					   {.Type = WindowEventType::Resized, .Window = WindowInstance.GetID(), .Extent = WindowInstance.StateData->Extent});
			   });
	}

	static void FramebufferSize(GLFWwindow *NativeWindow, int32 Width, int32 Height) noexcept
	{
		if (Width < 0 || Height < 0)
			return;
		Invoke(NativeWindow,
			   [Width, Height](Window &WindowInstance)
			   {
				   WindowInstance.StateData->FramebufferExtent = {static_cast<uint32>(Width), static_cast<uint32>(Height)};
				   WindowInstance.Publish({.Type = WindowEventType::FramebufferResized,
										   .Window = WindowInstance.GetID(),
										   .Extent = WindowInstance.StateData->FramebufferExtent});
			   });
	}

	static void ContentScale(GLFWwindow *NativeWindow, float32 XScale, float32 YScale) noexcept
	{
		Invoke(NativeWindow,
			   [XScale, YScale](Window &WindowInstance)
			   {
				   WindowInstance.StateData->ContentScale = std::max(XScale, YScale);
				   WindowInstance.Publish({.Type = WindowEventType::ContentScaleChanged,
										   .Window = WindowInstance.GetID(),
										   .ContentScaleX = XScale,
										   .ContentScaleY = YScale});
			   });
	}

	static void Focus(GLFWwindow *NativeWindow, int32 Focused) noexcept
	{
		Invoke(NativeWindow,
			   [Focused](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Focused = Focused == GLFW_TRUE;
				   if (!WindowInstance.StateData->Focused && WindowInstance.StateData->CursorMode == CursorMode::Confined)
					   ClipCursor(nullptr);
				   WindowInstance.Publish({.Type = WindowEventType::FocusChanged,
										   .Window = WindowInstance.GetID(),
										   .State = WindowInstance.StateData->Focused});
				   if (!WindowInstance.StateData->Focused)
					   WindowInstance.StateData->Manager->EnqueueInputEvent(
						   {.Type = input::InputEventType::FocusLost, .Window = WindowInstance.GetID()});
			   });
	}

	static void Iconify(GLFWwindow *NativeWindow, int32 Minimized) noexcept
	{
		Invoke(NativeWindow,
			   [Minimized](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Minimized = Minimized == GLFW_TRUE;
				   WindowInstance.Publish(
					   {.Type = WindowInstance.StateData->Minimized ? WindowEventType::Minimized : WindowEventType::Restored,
						.Window = WindowInstance.GetID(),
						.State = WindowInstance.StateData->Minimized});
			   });
	}

	static void Maximize(GLFWwindow *NativeWindow, int32 Maximized) noexcept
	{
		Invoke(NativeWindow,
			   [Maximized](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Maximized = Maximized == GLFW_TRUE;
				   WindowInstance.Publish(
					   {.Type = WindowInstance.StateData->Maximized ? WindowEventType::Maximized : WindowEventType::Restored,
						.Window = WindowInstance.GetID(),
						.State = WindowInstance.StateData->Maximized});
			   });
	}

	static void Key(GLFWwindow *NativeWindow, int32 Key, int32 ScanCode, int32 Action, int32 Modifiers) noexcept
	{
		const input::Key Translated = ToKey(Key);
		if (Translated == input::Key::Unknown)
			return;
		const input::InputState State = Action == GLFW_PRESS	? input::InputState::Pressed
										: Action == GLFW_REPEAT ? input::InputState::Repeated
																: input::InputState::Released;
		const uint32 NativeScanCode = ScanCode < 0 ? 0U : static_cast<uint32>(ScanCode);
		Invoke(
			NativeWindow,
			[Translated, NativeScanCode, State, Modifiers](Window &WindowInstance)
			{
				WindowInstance.StateData->Manager->EnqueueInputEvent(
					{.Type = input::InputEventType::Key,
					 .Window = WindowInstance.GetID(),
					 .Key = Translated,
					 .PhysicalKey = {.ScanCode = static_cast<uint16>(NativeScanCode & 0xFFU), .Extended = (NativeScanCode & 0x100U) != 0},
					 .State = State,
					 .Modifiers = ToModifiers(Modifiers)});
			});
	}

	static void Character(GLFWwindow *NativeWindow, uint32 Codepoint) noexcept
	{
		Invoke(NativeWindow,
			   [Codepoint](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Manager->EnqueueInputEvent(
					   {.Type = input::InputEventType::Text, .Window = WindowInstance.GetID(), .Codepoint = Codepoint});
			   });
	}

	static void MouseButton(GLFWwindow *NativeWindow, int32 Button, int32 Action, int32 Modifiers) noexcept
	{
		if (Button < GLFW_MOUSE_BUTTON_1 || Button > GLFW_MOUSE_BUTTON_8)
			return;
		Invoke(NativeWindow,
			   [Button, Action, Modifiers](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Manager->EnqueueInputEvent(
					   {.Type = input::InputEventType::MouseButton,
						.Window = WindowInstance.GetID(),
						.MouseButton = ToMouseButton(Button),
						.State = Action == GLFW_PRESS ? input::InputState::Pressed : input::InputState::Released,
						.Modifiers = ToModifiers(Modifiers)});
			   });
	}

	static void CursorPosition(GLFWwindow *NativeWindow, float64 X, float64 Y) noexcept
	{
		Invoke(NativeWindow,
			   [X, Y](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Manager->EnqueueInputEvent(
					   {.Type = input::InputEventType::MouseMove, .Window = WindowInstance.GetID(), .X = X, .Y = Y});
			   });
	}

	static void CursorEnter(GLFWwindow *NativeWindow, int32 Entered) noexcept
	{
		Invoke(NativeWindow,
			   [Entered](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Manager->EnqueueInputEvent(
					   {.Type = input::InputEventType::CursorEntered,
						.Window = WindowInstance.GetID(),
						.State = Entered == GLFW_TRUE ? input::InputState::Pressed : input::InputState::Released});
			   });
	}

	static void Scroll(GLFWwindow *NativeWindow, float64 X, float64 Y) noexcept
	{
		Invoke(NativeWindow,
			   [X, Y](Window &WindowInstance)
			   {
				   WindowInstance.StateData->Manager->EnqueueInputEvent(
					   {.Type = input::InputEventType::MouseScroll, .Window = WindowInstance.GetID(), .X = X, .Y = Y});
			   });
	}
};

Window::Window(WindowManager &Manager, const WindowID ID, const WindowSpecification &Specification, void *NativeWindow)
	: StateData(std::make_unique<State>())
{
	if (NativeWindow == nullptr)
		throw WindowException("Cannot construct Window without a native window");
	this->StateData->Manager = &Manager;
	this->StateData->NativeWindow = static_cast<GLFWwindow *>(NativeWindow);
	this->StateData->ID = ID;
	this->StateData->Title = Specification.Title;
	this->StateData->Extent = Specification.Extent;
	this->StateData->WindowedExtent = Specification.Extent;
	this->StateData->Constraints = Specification.Constraints;
	this->StateData->Mode = WindowMode::Windowed;
	this->StateData->SRGBPresentationCapable = glfwGetWindowAttrib(this->StateData->NativeWindow, GLFW_SRGB_CAPABLE) == GLFW_TRUE;

	glfwSetWindowUserPointer(this->StateData->NativeWindow, this);
	glfwSetWindowCloseCallback(this->StateData->NativeWindow, WindowCallbacks::Close);
	glfwSetWindowPosCallback(this->StateData->NativeWindow, WindowCallbacks::Position);
	glfwSetWindowSizeCallback(this->StateData->NativeWindow, WindowCallbacks::Size);
	glfwSetFramebufferSizeCallback(this->StateData->NativeWindow, WindowCallbacks::FramebufferSize);
	glfwSetWindowContentScaleCallback(this->StateData->NativeWindow, WindowCallbacks::ContentScale);
	glfwSetWindowFocusCallback(this->StateData->NativeWindow, WindowCallbacks::Focus);
	glfwSetWindowIconifyCallback(this->StateData->NativeWindow, WindowCallbacks::Iconify);
	glfwSetWindowMaximizeCallback(this->StateData->NativeWindow, WindowCallbacks::Maximize);
	glfwSetKeyCallback(this->StateData->NativeWindow, WindowCallbacks::Key);
	glfwSetCharCallback(this->StateData->NativeWindow, WindowCallbacks::Character);
	glfwSetMouseButtonCallback(this->StateData->NativeWindow, WindowCallbacks::MouseButton);
	glfwSetCursorPosCallback(this->StateData->NativeWindow, WindowCallbacks::CursorPosition);
	glfwSetCursorEnterCallback(this->StateData->NativeWindow, WindowCallbacks::CursorEnter);
	glfwSetScrollCallback(this->StateData->NativeWindow, WindowCallbacks::Scroll);

	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	if (SystemWindow == nullptr || SetWindowSubclass(SystemWindow, WindowCallbacks::NativeProcedure, static_cast<UINT_PTR>(ID.Value),
													 reinterpret_cast<DWORD_PTR>(this)) == FALSE)
	{
		throw WindowException("Failed to attach native window event routing");
	}
	this->StateData->NativeEventsAttached = true;
	if (WTSRegisterSessionNotification(SystemWindow, NOTIFY_FOR_THIS_SESSION) == FALSE)
	{
		RemoveWindowSubclass(SystemWindow, WindowCallbacks::NativeProcedure, static_cast<UINT_PTR>(ID.Value));
		this->StateData->NativeEventsAttached = false;
		throw WindowException("Failed to register session notifications");
	}
	(void)RegisterTouchWindow(SystemWindow, 0);
	GESTURECONFIG GestureConfiguration[]{{GID_ZOOM, GC_ZOOM, 0},
										 {GID_PAN, GC_PAN, 0},
										 {GID_ROTATE, GC_ROTATE, 0},
										 {GID_TWOFINGERTAP, GC_TWOFINGERTAP, 0},
										 {GID_PRESSANDTAP, GC_PRESSANDTAP, 0}};
	(void)SetGestureConfig(SystemWindow, 0, static_cast<UINT>(std::size(GestureConfiguration)), GestureConfiguration,
						   sizeof(GESTURECONFIG));
	try
	{
		this->InitializeDataTransfer();
	}
	catch (...)
	{
		UnregisterTouchWindow(SystemWindow);
		WTSUnRegisterSessionNotification(SystemWindow);
		RemoveWindowSubclass(SystemWindow, WindowCallbacks::NativeProcedure, static_cast<UINT_PTR>(ID.Value));
		this->StateData->NativeEventsAttached = false;
		throw;
	}
	this->StateData->Monitor = MonitorFromWindow(SystemWindow, MONITOR_DEFAULTTONEAREST);
	(void)this->SetDarkTheme(true);

	int32 X = 0;
	int32 Y = 0;
	int32 Width = 0;
	int32 Height = 0;
	glfwGetWindowPos(this->StateData->NativeWindow, &X, &Y);
	this->StateData->Position = {X, Y};
	this->StateData->WindowedPosition = this->StateData->Position;
	glfwGetFramebufferSize(this->StateData->NativeWindow, &Width, &Height);
	this->StateData->FramebufferExtent = {static_cast<uint32>(std::max(Width, 0)), static_cast<uint32>(std::max(Height, 0))};
	float32 XScale = 1.0f;
	float32 YScale = 1.0f;
	glfwGetWindowContentScale(this->StateData->NativeWindow, &XScale, &YScale);
	this->StateData->ContentScale = std::max(XScale, YScale);
	this->StateData->Visible = glfwGetWindowAttrib(this->StateData->NativeWindow, GLFW_VISIBLE) == GLFW_TRUE;
	this->StateData->Focused = glfwGetWindowAttrib(this->StateData->NativeWindow, GLFW_FOCUSED) == GLFW_TRUE;
	this->SetSizeConstraints(Specification.Constraints);
}

Window::~Window()
{
	if (this->StateData == nullptr || this->StateData->NativeWindow == nullptr)
		return;
	this->WaitForTrackedDialogs();
	this->ShutdownDataTransfer();
	if (this->StateData->Modal && this->StateData->Owner.IsValid())
	{
		if (Window *Owner = this->StateData->Manager->FindManagedWindow(this->StateData->Owner))
			EnableWindow(glfwGetWin32Window(static_cast<GLFWwindow *>(Owner->GetNativeWindow())), TRUE);
	}
	if (this->StateData->NotificationIconRegistered)
	{
		NOTIFYICONDATAW Notification{.cbSize = sizeof(NOTIFYICONDATAW),
									 .hWnd = glfwGetWin32Window(this->StateData->NativeWindow),
									 .uID = static_cast<UINT>(this->StateData->ID.Value)};
		Shell_NotifyIconW(NIM_DELETE, &Notification);
	}
	for (HICON Icon : this->StateData->ThumbnailIcons)
		if (Icon != nullptr)
			DestroyIcon(Icon);
	if (this->StateData->Taskbar != nullptr)
		this->StateData->Taskbar->Release();
	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	if (SystemWindow != nullptr)
	{
		WTSUnRegisterSessionNotification(SystemWindow);
		UnregisterTouchWindow(SystemWindow);
		if (this->StateData->NativeEventsAttached)
			RemoveWindowSubclass(SystemWindow, WindowCallbacks::NativeProcedure, static_cast<UINT_PTR>(this->StateData->ID.Value));
	}
	if (this->StateData->Cursor != nullptr)
		glfwDestroyCursor(this->StateData->Cursor);
	glfwSetWindowUserPointer(this->StateData->NativeWindow, nullptr);
	glfwDestroyWindow(this->StateData->NativeWindow);
	this->StateData->NativeWindow = nullptr;
}

WindowID Window::GetID() const noexcept
{
	return this->StateData->ID;
}
const std::string &Window::GetTitle() const noexcept
{
	return this->StateData->Title;
}
WindowExtent Window::GetExtent() const noexcept
{
	return this->StateData->Extent;
}
WindowExtent Window::GetFramebufferExtent() const noexcept
{
	return this->StateData->FramebufferExtent;
}
WindowPosition Window::GetPosition() const noexcept
{
	return this->StateData->Position;
}
float32 Window::GetContentScale() const noexcept
{
	return this->StateData->ContentScale;
}
WindowMode Window::GetMode() const noexcept
{
	return this->StateData->Mode;
}
bool Window::IsVisible() const noexcept
{
	return this->StateData->Visible;
}
bool Window::IsFocused() const noexcept
{
	return this->StateData->Focused;
}
bool Window::IsMinimized() const noexcept
{
	return this->StateData->Minimized;
}
bool Window::IsMaximized() const noexcept
{
	return this->StateData->Maximized;
}
bool Window::IsSRGBPresentationCapable() const noexcept
{
	return this->StateData->SRGBPresentationCapable;
}
bool Window::ShouldClose() const
{
	return glfwWindowShouldClose(this->StateData->NativeWindow) == GLFW_TRUE;
}
Context &Window::GetContext() const
{
	if (this->StateData->Context == nullptr)
		throw WindowException("Window has no Context");
	return *this->StateData->Context;
}
void Window::SetDataDropTarget(void *Target) noexcept
{
	this->StateData->DataDropTarget = Target;
}
void *Window::GetDataDropTarget() const noexcept
{
	return this->StateData->DataDropTarget;
}
void Window::RecordNativeFailure(std::exception_ptr Exception) noexcept
{
	this->StateData->Manager->RecordNativeException(std::move(Exception));
}
void Window::TrackDialog(std::shared_ptr<detail::DialogOperationBase> Operation)
{
	if (Operation == nullptr)
		throw WindowException("Cannot track an empty dialog operation");
	this->StateData->DialogOperations.erase(
		std::remove_if(this->StateData->DialogOperations.begin(), this->StateData->DialogOperations.end(),
					   [](const std::shared_ptr<detail::DialogOperationBase> &Candidate) { return Candidate.use_count() == 1; }),
		this->StateData->DialogOperations.end());
	this->StateData->DialogOperations.push_back(std::move(Operation));
}
void Window::WaitForTrackedDialogs() noexcept
{
	for (const std::shared_ptr<detail::DialogOperationBase> &Operation : this->StateData->DialogOperations)
		Operation->Wait();
	this->StateData->DialogOperations.clear();
}
void Window::PumpDialogEvents()
{
	this->StateData->Manager->WaitEvents(0.01);
}
void Window::RequireOwnerThread() const
{
	this->StateData->Manager->RequireOwnerThread();
}
WindowManager &Window::GetManager() const noexcept
{
	return *this->StateData->Manager;
}
void Window::RebindNativeRouting()
{
	glfwSetWindowUserPointer(this->StateData->NativeWindow, this);
	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	if (SystemWindow == nullptr ||
		SetWindowSubclass(SystemWindow, WindowCallbacks::NativeProcedure, static_cast<UINT_PTR>(this->StateData->ID.Value),
						  reinterpret_cast<DWORD_PTR>(this)) == FALSE)
	{
		throw WindowException("Failed to rebind native event routing after surface recreation");
	}
	this->RetargetDataTransfer();
}

void Window::SetTitle(std::string Title)
{
	this->RequireOwnerThread();
	if (Title.empty())
		throw WindowException("Window title cannot be empty");
	this->StateData->Title = std::move(Title);
	glfwSetWindowTitle(this->StateData->NativeWindow, this->StateData->Title.c_str());
}
void Window::SetExtent(const WindowExtent Extent)
{
	this->RequireOwnerThread();
	if (!Extent.IsValid())
		throw WindowException("Window extent must be non-zero");
	glfwSetWindowSize(this->StateData->NativeWindow, static_cast<int32>(Extent.Width), static_cast<int32>(Extent.Height));
}
void Window::SetPosition(const WindowPosition Position)
{
	this->RequireOwnerThread();
	glfwSetWindowPos(this->StateData->NativeWindow, Position.X, Position.Y);
}

void Window::SetSizeConstraints(const WindowSizeConstraints &Constraints)
{
	this->RequireOwnerThread();
	this->StateData->Constraints = Constraints;
	const int32 MinimumWidth = Constraints.Minimum ? static_cast<int32>(Constraints.Minimum->Width) : GLFW_DONT_CARE;
	const int32 MinimumHeight = Constraints.Minimum ? static_cast<int32>(Constraints.Minimum->Height) : GLFW_DONT_CARE;
	const int32 MaximumWidth = Constraints.Maximum ? static_cast<int32>(Constraints.Maximum->Width) : GLFW_DONT_CARE;
	const int32 MaximumHeight = Constraints.Maximum ? static_cast<int32>(Constraints.Maximum->Height) : GLFW_DONT_CARE;
	glfwSetWindowSizeLimits(this->StateData->NativeWindow, MinimumWidth, MinimumHeight, MaximumWidth, MaximumHeight);
	if (Constraints.AspectRatio)
		glfwSetWindowAspectRatio(this->StateData->NativeWindow, static_cast<int32>(Constraints.AspectRatio->Width),
								 static_cast<int32>(Constraints.AspectRatio->Height));
	else
		glfwSetWindowAspectRatio(this->StateData->NativeWindow, GLFW_DONT_CARE, GLFW_DONT_CARE);
}

void Window::SetMode(const WindowMode Mode)
{
	this->RequireOwnerThread();
	if (Mode == this->StateData->Mode)
		return;
	if (this->StateData->Mode == WindowMode::Windowed)
	{
		this->StateData->WindowedPosition = this->StateData->Position;
		this->StateData->WindowedExtent = this->StateData->Extent;
	}
	GLFWmonitor *Monitor = this->StateData->TargetMonitor == nullptr ? glfwGetPrimaryMonitor() : this->StateData->TargetMonitor;
	if (Monitor == nullptr)
		throw WindowException("No monitor is available for mode transition");
	const GLFWvidmode *VideoMode = glfwGetVideoMode(Monitor);
	if (VideoMode == nullptr)
		throw WindowException("Cannot query monitor video mode");
	if (Mode == WindowMode::ExclusiveFullscreen && this->StateData->RequestedVideoMode)
	{
		int32 ModeCount = 0;
		const GLFWvidmode *Modes = glfwGetVideoModes(Monitor, &ModeCount);
		if (Modes == nullptr || ModeCount <= 0)
			throw WindowException("Cannot enumerate exclusive-fullscreen monitor modes");
		const MonitorVideoMode Requested = *this->StateData->RequestedVideoMode;
		const auto Match = std::find_if(Modes, Modes + ModeCount,
										[&Requested](const GLFWvidmode &Candidate)
										{
											return Candidate.width == static_cast<int32>(Requested.Extent.Width) &&
												   Candidate.height == static_cast<int32>(Requested.Extent.Height) &&
												   Candidate.redBits == static_cast<int32>(Requested.RedBits) &&
												   Candidate.greenBits == static_cast<int32>(Requested.GreenBits) &&
												   Candidate.blueBits == static_cast<int32>(Requested.BlueBits) &&
												   Candidate.refreshRate == static_cast<int32>(Requested.RefreshRate);
										});
		if (Match == Modes + ModeCount)
			throw WindowException("Requested exclusive-fullscreen monitor mode is unavailable");
		VideoMode = Match;
	}
	if (Mode == WindowMode::Windowed)
	{
		glfwSetWindowAttrib(this->StateData->NativeWindow, GLFW_DECORATED, GLFW_TRUE);
		glfwSetWindowMonitor(this->StateData->NativeWindow, nullptr, this->StateData->WindowedPosition.X,
							 this->StateData->WindowedPosition.Y, static_cast<int32>(this->StateData->WindowedExtent.Width),
							 static_cast<int32>(this->StateData->WindowedExtent.Height), GLFW_DONT_CARE);
	}
	else if (Mode == WindowMode::Borderless)
	{
		int32 MonitorX = 0;
		int32 MonitorY = 0;
		glfwGetMonitorPos(Monitor, &MonitorX, &MonitorY);
		glfwSetWindowAttrib(this->StateData->NativeWindow, GLFW_DECORATED, GLFW_FALSE);
		glfwSetWindowMonitor(this->StateData->NativeWindow, nullptr, MonitorX, MonitorY, VideoMode->width, VideoMode->height,
							 GLFW_DONT_CARE);
	}
	else
	{
		glfwSetWindowMonitor(this->StateData->NativeWindow, Monitor, 0, 0, VideoMode->width, VideoMode->height, VideoMode->refreshRate);
	}
	this->StateData->Mode = Mode;
}

void Window::SetMonitor(const MonitorID &Monitor, const WindowMode Mode, std::optional<MonitorVideoMode> VideoMode)
{
	this->RequireOwnerThread();
	if (VideoMode && Mode != WindowMode::ExclusiveFullscreen)
		throw WindowException("An explicit monitor video mode is only valid for exclusive fullscreen");
	GLFWmonitor *NativeMonitor = static_cast<GLFWmonitor *>(this->StateData->Manager->FindNativeMonitor(Monitor));
	this->StateData->TargetMonitor = NativeMonitor;
	this->StateData->RequestedVideoMode = std::move(VideoMode);
	if (Mode == WindowMode::Windowed)
	{
		int32 WorkX = 0, WorkY = 0, WorkWidth = 0, WorkHeight = 0;
		glfwGetMonitorWorkarea(NativeMonitor, &WorkX, &WorkY, &WorkWidth, &WorkHeight);
		this->StateData->WindowedPosition = {WorkX, WorkY};
	}
	const WindowMode PreviousMode = this->StateData->Mode;
	this->StateData->Mode = Mode == WindowMode::Windowed ? WindowMode::Borderless : WindowMode::Windowed;
	try
	{
		this->SetMode(Mode);
	}
	catch (...)
	{
		this->StateData->Mode = PreviousMode;
		throw;
	}
}

void Window::SetVisible(const bool Visible)
{
	this->RequireOwnerThread();
	if (Visible)
		glfwShowWindow(this->StateData->NativeWindow);
	else
		glfwHideWindow(this->StateData->NativeWindow);
	this->StateData->Visible = Visible;
	this->Publish({.Type = WindowEventType::VisibilityChanged, .Window = this->GetID(), .State = Visible});
}
void Window::SetEnabled(const bool Enabled)
{
	this->RequireOwnerThread();
	EnableWindow(glfwGetWin32Window(this->StateData->NativeWindow), Enabled ? TRUE : FALSE);
}
void Window::SetDecorated(const bool Decorated)
{
	this->RequireOwnerThread();
	glfwSetWindowAttrib(this->StateData->NativeWindow, GLFW_DECORATED, Decorated ? GLFW_TRUE : GLFW_FALSE);
}
void Window::SetResizable(const bool Resizable)
{
	this->RequireOwnerThread();
	glfwSetWindowAttrib(this->StateData->NativeWindow, GLFW_RESIZABLE, Resizable ? GLFW_TRUE : GLFW_FALSE);
}
void Window::SetTopmost(const bool Topmost)
{
	this->RequireOwnerThread();
	glfwSetWindowAttrib(this->StateData->NativeWindow, GLFW_FLOATING, Topmost ? GLFW_TRUE : GLFW_FALSE);
}
void Window::SetTaskbarVisible(const bool Visible)
{
	this->RequireOwnerThread();
	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	LONG_PTR ExtendedStyle = GetWindowLongPtrW(SystemWindow, GWL_EXSTYLE);
	if (Visible)
	{
		ExtendedStyle = (ExtendedStyle | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW;
	}
	else
	{
		ExtendedStyle = (ExtendedStyle | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
	}
	SetLastError(ERROR_SUCCESS);
	if (SetWindowLongPtrW(SystemWindow, GWL_EXSTYLE, ExtendedStyle) == 0 && GetLastError() != ERROR_SUCCESS)
		throw WindowException("Failed to update taskbar visibility");
	if (SetWindowPos(SystemWindow, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ==
		FALSE)
		throw WindowException("Failed to apply taskbar visibility");
}

void Window::SetToolWindow(const bool Enabled)
{
	this->RequireOwnerThread();
	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	LONG_PTR ExtendedStyle = GetWindowLongPtrW(SystemWindow, GWL_EXSTYLE);
	if (Enabled)
		ExtendedStyle |= WS_EX_TOOLWINDOW;
	else
		ExtendedStyle &= ~WS_EX_TOOLWINDOW;
	SetLastError(ERROR_SUCCESS);
	if (SetWindowLongPtrW(SystemWindow, GWL_EXSTYLE, ExtendedStyle) == 0 && GetLastError() != ERROR_SUCCESS)
		throw WindowException("Failed to update tool-window behavior");
	if (SetWindowPos(SystemWindow, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ==
		FALSE)
		throw WindowException("Failed to apply tool-window behavior");
}

void Window::SetOwner(Window *Owner)
{
	this->RequireOwnerThread();
	if (Owner == this)
		throw WindowException("Window cannot own itself");
	if (this->StateData->Modal)
		throw WindowException("Window ownership cannot change while modal");
	if (Owner != nullptr && &Owner->GetManager() != this->StateData->Manager)
		throw WindowException("Window ownership cannot cross WindowManager instances");
	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	HWND OwnerWindow = Owner == nullptr ? nullptr : glfwGetWin32Window(static_cast<GLFWwindow *>(Owner->GetNativeWindow()));
	SetLastError(ERROR_SUCCESS);
	if (SetWindowLongPtrW(SystemWindow, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(OwnerWindow)) == 0 && GetLastError() != ERROR_SUCCESS)
		throw WindowException("Failed to update window ownership");
	this->StateData->Owner = Owner == nullptr ? WindowID{} : Owner->GetID();
}

void Window::DetachFromOwner(const WindowID Owner) noexcept
{
	if (this->StateData->Owner != Owner)
		return;
	this->StateData->Modal = false;
	this->StateData->Owner = {};
	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	if (SystemWindow != nullptr)
		(void)SetWindowLongPtrW(SystemWindow, GWLP_HWNDPARENT, 0);
}

void Window::SetModal(const bool Modal)
{
	this->RequireOwnerThread();
	if (Modal == this->StateData->Modal)
		return;
	if (Modal && !this->StateData->Owner.IsValid())
		throw WindowException("A modal Window requires an owner");
	if (this->StateData->Owner.IsValid())
	{
		Window *Owner = this->StateData->Manager->FindManagedWindow(this->StateData->Owner);
		if (Owner == nullptr)
			throw WindowException("A modal Window's owner is no longer managed");
		Owner->SetEnabled(!Modal);
	}
	this->StateData->Modal = Modal;
	if (Modal)
		this->RequestFocus();
}

void Window::SetZOrder(const WindowZOrder Order, Window *RelativeTo)
{
	this->RequireOwnerThread();
	if (RelativeTo == this)
		throw WindowException("Window cannot be positioned relative to itself");
	HWND InsertAfter = HWND_NOTOPMOST;
	if (RelativeTo != nullptr)
		InsertAfter = glfwGetWin32Window(static_cast<GLFWwindow *>(RelativeTo->GetNativeWindow()));
	else if (Order == WindowZOrder::Bottom)
		InsertAfter = HWND_BOTTOM;
	else if (Order == WindowZOrder::Top)
		InsertAfter = HWND_TOP;
	else if (Order == WindowZOrder::Topmost)
		InsertAfter = HWND_TOPMOST;
	if (SetWindowPos(glfwGetWin32Window(this->StateData->NativeWindow), InsertAfter, 0, 0, 0, 0,
					 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) == FALSE)
		throw WindowException("Failed to update window Z-order");
}
void Window::SetOpacity(const float32 Opacity)
{
	this->RequireOwnerThread();
	if (Opacity < 0.0f || Opacity > 1.0f)
		throw WindowException("Window opacity must be in the [0, 1] range");
	glfwSetWindowOpacity(this->StateData->NativeWindow, Opacity);
}
void Window::SetMousePassthrough(const bool Enabled)
{
	this->RequireOwnerThread();
	glfwSetWindowAttrib(this->StateData->NativeWindow, GLFW_MOUSE_PASSTHROUGH, Enabled ? GLFW_TRUE : GLFW_FALSE);
}

void Window::SetCursorMode(const CursorMode Mode)
{
	this->RequireOwnerThread();
	if (this->StateData->CursorMode == CursorMode::Confined && Mode != CursorMode::Confined)
		ClipCursor(nullptr);
	if (Mode == CursorMode::Visible)
		glfwSetInputMode(this->StateData->NativeWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	else if (Mode == CursorMode::Hidden)
		glfwSetInputMode(this->StateData->NativeWindow, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
	else if (Mode == CursorMode::Relative)
		glfwSetInputMode(this->StateData->NativeWindow, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	else if (Mode == CursorMode::Captured)
		glfwSetInputMode(this->StateData->NativeWindow, GLFW_CURSOR, GLFW_CURSOR_CAPTURED);
	else
	{
		glfwSetInputMode(this->StateData->NativeWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		if (this->StateData->Focused)
		{
			HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
			RECT ClientRectangle{};
			if (SystemWindow == nullptr || GetClientRect(SystemWindow, &ClientRectangle) == FALSE)
				throw WindowException("Failed to query cursor confinement rectangle");
			POINT TopLeft{ClientRectangle.left, ClientRectangle.top};
			POINT BottomRight{ClientRectangle.right, ClientRectangle.bottom};
			if (ClientToScreen(SystemWindow, &TopLeft) == FALSE || ClientToScreen(SystemWindow, &BottomRight) == FALSE)
				throw WindowException("Failed to map cursor confinement rectangle");
			RECT Confinement{TopLeft.x, TopLeft.y, BottomRight.x, BottomRight.y};
			if (ClipCursor(&Confinement) == FALSE)
				throw WindowException("Failed to confine the cursor");
		}
		else
			ClipCursor(nullptr);
	}
	if (Mode == CursorMode::Relative)
	{
		if (glfwRawMouseMotionSupported() != GLFW_TRUE)
			throw WindowException("Raw relative mouse movement is not supported");
		glfwSetInputMode(this->StateData->NativeWindow, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	}
	else if (glfwRawMouseMotionSupported() == GLFW_TRUE)
		glfwSetInputMode(this->StateData->NativeWindow, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
	this->StateData->CursorMode = Mode;
}

void Window::SetCursorShape(const CursorShape Shape)
{
	this->RequireOwnerThread();
	int32 NativeShape = GLFW_ARROW_CURSOR;
	if (Shape == CursorShape::Text)
		NativeShape = GLFW_IBEAM_CURSOR;
	else if (Shape == CursorShape::Crosshair)
		NativeShape = GLFW_CROSSHAIR_CURSOR;
	else if (Shape == CursorShape::PointingHand)
		NativeShape = GLFW_POINTING_HAND_CURSOR;
	else if (Shape == CursorShape::ResizeHorizontal)
		NativeShape = GLFW_RESIZE_EW_CURSOR;
	else if (Shape == CursorShape::ResizeVertical)
		NativeShape = GLFW_RESIZE_NS_CURSOR;
	else if (Shape == CursorShape::ResizeNorthwestSoutheast)
		NativeShape = GLFW_RESIZE_NWSE_CURSOR;
	else if (Shape == CursorShape::ResizeNortheastSouthwest)
		NativeShape = GLFW_RESIZE_NESW_CURSOR;
	else if (Shape == CursorShape::ResizeAll)
		NativeShape = GLFW_RESIZE_ALL_CURSOR;
	else if (Shape == CursorShape::NotAllowed)
		NativeShape = GLFW_NOT_ALLOWED_CURSOR;
	GLFWcursor *Replacement = glfwCreateStandardCursor(NativeShape);
	if (Replacement == nullptr)
		throw WindowException("Failed to create the requested cursor shape");
	glfwSetCursor(this->StateData->NativeWindow, Replacement);
	if (this->StateData->Cursor != nullptr)
		glfwDestroyCursor(this->StateData->Cursor);
	this->StateData->Cursor = Replacement;
}

void Window::SetCustomCursor(const WindowImageView &Image, const WindowPosition HotSpot)
{
	this->RequireOwnerThread();
	if (!Image.IsValid())
		throw WindowException("Custom cursor requires a valid RGBA8 image");
	if (HotSpot.X < 0 || HotSpot.Y < 0 || static_cast<uint32>(HotSpot.X) >= Image.Extent.Width ||
		static_cast<uint32>(HotSpot.Y) >= Image.Extent.Height)
		throw WindowException("Custom cursor hot spot lies outside the image");
	GLFWimage NativeImage{static_cast<int32>(Image.Extent.Width), static_cast<int32>(Image.Extent.Height),
						  const_cast<uint8 *>(Image.Pixels)};
	GLFWcursor *Replacement = glfwCreateCursor(&NativeImage, HotSpot.X, HotSpot.Y);
	if (Replacement == nullptr)
		throw WindowException("Failed to create custom cursor");
	glfwSetCursor(this->StateData->NativeWindow, Replacement);
	if (this->StateData->Cursor != nullptr)
		glfwDestroyCursor(this->StateData->Cursor);
	this->StateData->Cursor = Replacement;
}

void Window::SetCustomCursor(const resource::AssetHandle<resource::Texture2DAsset> &Image, const WindowPosition HotSpot)
{
	this->RequireOwnerThread();
	auto PinnedImage = Image.TryPin();
	if (!PinnedImage || PinnedImage->GetChannels() != 4)
		throw WindowException("Custom cursor asset requires resident CPU RGBA8 pixels");
	this->SetCustomCursor({.Pixels = PinnedImage->GetPixels().data(),
						   .Extent = {static_cast<uint32>(PinnedImage->GetWidth()), static_cast<uint32>(PinnedImage->GetHeight())}},
						  HotSpot);
}

void Window::SetIcon(const WindowImageView &Image)
{
	this->RequireOwnerThread();
	if (!Image.IsValid())
		throw WindowException("Window icon requires non-null RGBA8 pixels and a non-zero extent");
	GLFWimage NativeImage{static_cast<int32>(Image.Extent.Width), static_cast<int32>(Image.Extent.Height),
						  const_cast<uint8 *>(Image.Pixels)};
	glfwSetWindowIcon(this->StateData->NativeWindow, 1, &NativeImage);
}

void Window::SetIcon(const resource::AssetHandle<resource::Texture2DAsset> &Image)
{
	this->RequireOwnerThread();
	auto PinnedImage = Image.TryPin();
	if (!PinnedImage || PinnedImage->GetChannels() != 4)
		throw WindowException("Window icon asset requires resident CPU RGBA8 pixels");
	this->SetIcon({.Pixels = PinnedImage->GetPixels().data(),
				   .Extent = {static_cast<uint32>(PinnedImage->GetWidth()), static_cast<uint32>(PinnedImage->GetHeight())}});
}

WindowFeatureResult Window::SetDarkTheme(const bool Enabled)
{
	this->RequireOwnerThread();
	const BOOL Value = Enabled ? TRUE : FALSE;
	const HRESULT Result =
		DwmSetWindowAttribute(glfwGetWin32Window(this->StateData->NativeWindow), DWMWA_USE_IMMERSIVE_DARK_MODE, &Value, sizeof(Value));
	if (Result == E_INVALIDARG)
		return WindowFeatureResult::Unsupported;
	if (FAILED(Result))
		throw WindowException("Failed to update dark window theme with HRESULT " + std::to_string(static_cast<uint32>(Result)));
	return WindowFeatureResult::Applied;
}

WindowFeatureResult Window::SetCornerPreference(const WindowCornerPreference Preference)
{
	this->RequireOwnerThread();
	DWM_WINDOW_CORNER_PREFERENCE NativePreference = DWMWCP_DEFAULT;
	if (Preference == WindowCornerPreference::Square)
		NativePreference = DWMWCP_DONOTROUND;
	else if (Preference == WindowCornerPreference::Rounded)
		NativePreference = DWMWCP_ROUND;
	else if (Preference == WindowCornerPreference::RoundedSmall)
		NativePreference = DWMWCP_ROUNDSMALL;
	const HRESULT Result = DwmSetWindowAttribute(glfwGetWin32Window(this->StateData->NativeWindow), DWMWA_WINDOW_CORNER_PREFERENCE,
												 &NativePreference, sizeof(NativePreference));
	if (Result == E_INVALIDARG)
		return WindowFeatureResult::Unsupported;
	if (FAILED(Result))
		throw WindowException("Failed to update window corner preference with HRESULT " + std::to_string(static_cast<uint32>(Result)));
	return WindowFeatureResult::Applied;
}

WindowFeatureResult Window::SetBackdrop(const WindowBackdrop Backdrop)
{
	this->RequireOwnerThread();
	DWM_SYSTEMBACKDROP_TYPE NativeBackdrop = DWMSBT_NONE;
	if (Backdrop == WindowBackdrop::Mica)
		NativeBackdrop = DWMSBT_MAINWINDOW;
	else if (Backdrop == WindowBackdrop::Acrylic)
		NativeBackdrop = DWMSBT_TRANSIENTWINDOW;
	else if (Backdrop == WindowBackdrop::Tabbed)
		NativeBackdrop = DWMSBT_TABBEDWINDOW;
	const HRESULT Result = DwmSetWindowAttribute(glfwGetWin32Window(this->StateData->NativeWindow), DWMWA_SYSTEMBACKDROP_TYPE,
												 &NativeBackdrop, sizeof(NativeBackdrop));
	if (Result == E_INVALIDARG || Result == DWM_E_COMPOSITIONDISABLED)
		return WindowFeatureResult::Unsupported;
	if (FAILED(Result))
		throw WindowException("Failed to update window backdrop with HRESULT " + std::to_string(static_cast<uint32>(Result)));
	return WindowFeatureResult::Applied;
}

void Window::SetTitleBar(const TitleBarSpecification &Specification, WindowHitTest HitTest)
{
	this->RequireOwnerThread();
	if (Specification.Custom && Specification.DraggableHeight == 0)
		throw WindowException("Custom title bar height must be non-zero");
	this->StateData->TitleBar = Specification;
	this->StateData->HitTest = std::move(HitTest);
	if (SetWindowPos(glfwGetWin32Window(this->StateData->NativeWindow), nullptr, 0, 0, 0, 0,
					 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) == FALSE)
		throw WindowException("Failed to apply custom title-bar configuration");
}

void Window::SetSystemMenuCommandEnabled(const WindowSystemCommand Command, const bool Enabled)
{
	this->RequireOwnerThread();
	uint32 NativeCommand = SC_CLOSE;
	if (Command == WindowSystemCommand::Restore)
		NativeCommand = SC_RESTORE;
	else if (Command == WindowSystemCommand::Move)
		NativeCommand = SC_MOVE;
	else if (Command == WindowSystemCommand::Size)
		NativeCommand = SC_SIZE;
	else if (Command == WindowSystemCommand::Minimize)
		NativeCommand = SC_MINIMIZE;
	else if (Command == WindowSystemCommand::Maximize)
		NativeCommand = SC_MAXIMIZE;
	HMENU Menu = GetSystemMenu(glfwGetWin32Window(this->StateData->NativeWindow), FALSE);
	if (Menu == nullptr || EnableMenuItem(Menu, NativeCommand, MF_BYCOMMAND | (Enabled ? MF_ENABLED : MF_GRAYED)) == static_cast<UINT>(-1))
		throw WindowException("Failed to update system-menu command");
}

void Window::ShowSystemMenu(const WindowPosition ScreenPosition)
{
	this->RequireOwnerThread();
	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	HMENU Menu = GetSystemMenu(SystemWindow, FALSE);
	if (Menu == nullptr)
		throw WindowException("Window has no system menu");
	const uint32 Command =
		TrackPopupMenu(Menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ScreenPosition.X, ScreenPosition.Y, 0, SystemWindow, nullptr);
	if (Command != 0)
		PostMessageW(SystemWindow, WM_SYSCOMMAND, Command, 0);
}

void *Window::GetTaskbarService()
{
	if (this->StateData->Taskbar != nullptr)
		return this->StateData->Taskbar;
	ITaskbarList3 *Taskbar = nullptr;
	const HRESULT Creation = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Taskbar));
	if (FAILED(Creation))
		throw WindowException("Taskbar service creation failed with HRESULT " + std::to_string(static_cast<uint32>(Creation)));
	const HRESULT Initialization = Taskbar->HrInit();
	if (FAILED(Initialization))
	{
		Taskbar->Release();
		throw WindowException("Taskbar service initialization failed with HRESULT " + std::to_string(static_cast<uint32>(Initialization)));
	}
	this->StateData->Taskbar = Taskbar;
	return Taskbar;
}

void Window::SetTaskbarProgress(const TaskbarProgressState State, const uint64 Completed, const uint64 Total)
{
	this->RequireOwnerThread();
	if ((State == TaskbarProgressState::Normal || State == TaskbarProgressState::Error || State == TaskbarProgressState::Paused) &&
		(Total == 0 || Completed > Total))
		throw WindowException("Taskbar progress requires completed <= non-zero total");
	TBPFLAG NativeState = TBPF_NOPROGRESS;
	if (State == TaskbarProgressState::Indeterminate)
		NativeState = TBPF_INDETERMINATE;
	else if (State == TaskbarProgressState::Normal)
		NativeState = TBPF_NORMAL;
	else if (State == TaskbarProgressState::Error)
		NativeState = TBPF_ERROR;
	else if (State == TaskbarProgressState::Paused)
		NativeState = TBPF_PAUSED;
	ITaskbarList3 *Taskbar = static_cast<ITaskbarList3 *>(this->GetTaskbarService());
	HWND SystemWindow = glfwGetWin32Window(this->StateData->NativeWindow);
	HRESULT Result = Taskbar->SetProgressState(SystemWindow, NativeState);
	if (SUCCEEDED(Result) && NativeState != TBPF_NOPROGRESS && NativeState != TBPF_INDETERMINATE)
		Result = Taskbar->SetProgressValue(SystemWindow, Completed, Total);
	if (FAILED(Result))
		throw WindowException("Taskbar progress update failed with HRESULT " + std::to_string(static_cast<uint32>(Result)));
}

void Window::SetTaskbarOverlay(const WindowImageView &Image, std::string Description)
{
	this->RequireOwnerThread();
	HICON Icon = CreateIcon(Image);
	const std::wstring NativeDescription = ToWide(Description);
	const HRESULT Result = static_cast<ITaskbarList3 *>(this->GetTaskbarService())
							   ->SetOverlayIcon(glfwGetWin32Window(this->StateData->NativeWindow), Icon, NativeDescription.c_str());
	DestroyIcon(Icon);
	if (FAILED(Result))
		throw WindowException("Taskbar overlay update failed with HRESULT " + std::to_string(static_cast<uint32>(Result)));
}

void Window::ClearTaskbarOverlay()
{
	this->RequireOwnerThread();
	const HRESULT Result = static_cast<ITaskbarList3 *>(this->GetTaskbarService())
							   ->SetOverlayIcon(glfwGetWin32Window(this->StateData->NativeWindow), nullptr, L"");
	if (FAILED(Result))
		throw WindowException("Taskbar overlay removal failed with HRESULT " + std::to_string(static_cast<uint32>(Result)));
}

void Window::SetThumbnailActions(const std::span<const ThumbnailAction> Actions)
{
	this->RequireOwnerThread();
	if (Actions.size() > this->StateData->ThumbnailCommands.size())
		throw WindowException("A Window supports at most seven thumbnail actions");
	for (usize Index = 0; Index < Actions.size(); ++Index)
	{
		if (Actions[Index].ID == 0 || Actions[Index].Title.empty())
			throw WindowException("Thumbnail actions require non-zero IDs and titles");
		if (std::count_if(Actions.begin(), Actions.end(),
						  [&Actions, Index](const ThumbnailAction &Action) { return Action.ID == Actions[Index].ID; }) != 1)
			throw WindowException("Thumbnail action IDs must be unique");
	}
	for (HICON Icon : this->StateData->ThumbnailIcons)
		if (Icon != nullptr)
			DestroyIcon(Icon);
	this->StateData->ThumbnailIcons.clear();
	this->StateData->ThumbnailCommands.fill(0);
	std::array<THUMBBUTTON, 7> Buttons{};
	for (usize Index = 0; Index < Buttons.size(); ++Index)
	{
		THUMBBUTTON &Button = Buttons[Index];
		Button.dwMask = THB_FLAGS | THB_TOOLTIP;
		Button.iId = static_cast<UINT>(Index + 1);
		if (Index >= Actions.size())
		{
			Button.dwFlags = THBF_HIDDEN;
			continue;
		}
		const ThumbnailAction &Action = Actions[Index];
		this->StateData->ThumbnailCommands[Index] = Action.ID;
		const std::wstring Title = ToWide(Action.Title);
		wcsncpy_s(Button.szTip, Title.c_str(), _TRUNCATE);
		Button.dwFlags = Action.Enabled ? THBF_ENABLED : THBF_DISABLED;
		if (Action.DismissOnClick)
			Button.dwFlags = static_cast<THUMBBUTTONFLAGS>(Button.dwFlags | THBF_DISMISSONCLICK);
		if (Action.NoBackground)
			Button.dwFlags = static_cast<THUMBBUTTONFLAGS>(Button.dwFlags | THBF_NOBACKGROUND);
		if (Action.Icon)
		{
			Button.hIcon = CreateIcon(*Action.Icon);
			Button.dwMask = static_cast<THUMBBUTTONMASK>(Button.dwMask | THB_ICON);
			this->StateData->ThumbnailIcons.push_back(Button.hIcon);
		}
	}
	ITaskbarList3 *Taskbar = static_cast<ITaskbarList3 *>(this->GetTaskbarService());
	const HRESULT Result = this->StateData->ThumbnailButtonsAdded
							   ? Taskbar->ThumbBarUpdateButtons(glfwGetWin32Window(this->StateData->NativeWindow),
																static_cast<UINT>(Buttons.size()), Buttons.data())
							   : Taskbar->ThumbBarAddButtons(glfwGetWin32Window(this->StateData->NativeWindow),
															 static_cast<UINT>(Buttons.size()), Buttons.data());
	if (FAILED(Result))
		throw WindowException("Thumbnail action update failed with HRESULT " + std::to_string(static_cast<uint32>(Result)));
	this->StateData->ThumbnailButtonsAdded = true;
}

void Window::ShowNotification(const WindowNotification &Notification)
{
	this->RequireOwnerThread();
	if (Notification.Title.empty() || Notification.Message.empty())
		throw WindowException("Notification title and message cannot be empty");
	NOTIFYICONDATAW Data{};
	Data.cbSize = sizeof(Data);
	Data.hWnd = glfwGetWin32Window(this->StateData->NativeWindow);
	Data.uID = static_cast<UINT>(this->StateData->ID.Value);
	Data.uFlags = NIF_ICON | NIF_TIP;
	Data.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
	const std::wstring Tip = ToWide(this->StateData->Title);
	wcsncpy_s(Data.szTip, Tip.c_str(), _TRUNCATE);
	if (!this->StateData->NotificationIconRegistered)
	{
		if (Shell_NotifyIconW(NIM_ADD, &Data) == FALSE)
			throw WindowException("Failed to register notification identity");
		this->StateData->NotificationIconRegistered = true;
		Data.uVersion = NOTIFYICON_VERSION_4;
		(void)Shell_NotifyIconW(NIM_SETVERSION, &Data);
	}
	Data.uFlags = NIF_INFO;
	const std::wstring Title = ToWide(Notification.Title);
	const std::wstring Message = ToWide(Notification.Message);
	wcsncpy_s(Data.szInfoTitle, Title.c_str(), _TRUNCATE);
	wcsncpy_s(Data.szInfo, Message.c_str(), _TRUNCATE);
	Data.dwInfoFlags = Notification.Severity == NotificationSeverity::Warning ? NIIF_WARNING
					   : Notification.Severity == NotificationSeverity::Error ? NIIF_ERROR
																			  : NIIF_INFO;
	if (Shell_NotifyIconW(NIM_MODIFY, &Data) == FALSE)
		throw WindowException("Failed to show notification");
}

void Window::RequestFocus()
{
	this->RequireOwnerThread();
	glfwFocusWindow(this->StateData->NativeWindow);
}
void Window::RequestAttention()
{
	this->RequireOwnerThread();
	glfwRequestWindowAttention(this->StateData->NativeWindow);
}
void Window::RequestClose()
{
	this->RequireOwnerThread();
	glfwSetWindowShouldClose(this->StateData->NativeWindow, GLFW_TRUE);
	this->Publish({.Type = WindowEventType::CloseRequested, .Window = this->GetID()});
}
void Window::CancelClose()
{
	this->RequireOwnerThread();
	glfwSetWindowShouldClose(this->StateData->NativeWindow, GLFW_FALSE);
}
void Window::Minimize()
{
	this->RequireOwnerThread();
	glfwIconifyWindow(this->StateData->NativeWindow);
}
void Window::Maximize()
{
	this->RequireOwnerThread();
	glfwMaximizeWindow(this->StateData->NativeWindow);
}
void Window::Restore()
{
	this->RequireOwnerThread();
	glfwRestoreWindow(this->StateData->NativeWindow);
}

PresentationResult Window::SetPresentationMode(const PresentationMode Mode)
{
	Context &Context = this->GetContext();
	Context.RequireCurrentThread();
	if (!Context.IsCurrent())
		throw WindowException("Presentation mode requires this Window's Context to be current");
	if (Mode == PresentationMode::Off)
		glfwSwapInterval(0);
	else if (Mode == PresentationMode::On)
		glfwSwapInterval(1);
	else
	{
		if (glfwExtensionSupported("WGL_EXT_swap_control_tear") == GLFW_TRUE)
			glfwSwapInterval(-1);
		else
		{
			glfwSwapInterval(1);
			this->StateData->PresentationMode = PresentationMode::On;
			return PresentationResult::AppliedWithFallback;
		}
	}
	this->StateData->PresentationMode = Mode;
	return PresentationResult::Applied;
}

void Window::Present()
{
	Context &Context = this->GetContext();
	Context.RequireCurrentThread();
	if (!Context.IsCurrent())
		throw WindowException("Present requires this Window's Context to be current");
	glfwSwapBuffers(this->StateData->NativeWindow);
}
void *Window::GetNativeWindow() const noexcept
{
	return this->StateData->NativeWindow;
}
void Window::SetContext(Context &Context) noexcept
{
	this->StateData->Context = &Context;
}
void Window::Publish(WindowEvent Event)
{
	this->StateData->Manager->EnqueueWindowEvent(std::move(Event));
}
} // namespace core
