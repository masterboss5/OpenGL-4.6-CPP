#include "WindowManager.h"
#include "src/concepts.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <GL/glew.h>
#include <GL/wglew.h>
#include <Windows.h>
#include <objbase.h>
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef FindWindow
#undef FindWindow
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include "Context.h"
#include "WindowException.h"
#include "src/pipeline/device/Device.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace core
{
struct WindowManager::ContextGroup final
{
	std::string Name;
	GLFWwindow *ShareRootWindow = nullptr;
	std::unique_ptr<Context> AnchorContext;
	std::unique_ptr<pipeline::device::Device> Device;
	std::unordered_map<WindowID, std::unique_ptr<Context>> WindowContexts;
};

namespace
{
[[nodiscard]] std::string GetGLFWDiagnostic(const std::string_view Operation)
{
	const char *Description = nullptr;
	const int32 Code = glfwGetError(&Description);
	std::ostringstream Diagnostic;
	Diagnostic << Operation;
	if (Code != GLFW_NO_ERROR)
		Diagnostic << " failed with GLFW error " << Code << ": " << (Description == nullptr ? "no diagnostic" : Description);
	return Diagnostic.str();
}

[[nodiscard]] bool IsReplaceableEvent(const WindowEventType Type) noexcept
{
	return Type == WindowEventType::Moved || Type == WindowEventType::Resized || Type == WindowEventType::FramebufferResized ||
		   Type == WindowEventType::ContentScaleChanged;
}

template <FunctionPointer Procedure> [[nodiscard]] Procedure RequireWGLProcedure(const char *Name)
{
	const PROC Address = wglGetProcAddress(Name);
	if (Address == nullptr || Address == reinterpret_cast<PROC>(1) || Address == reinterpret_cast<PROC>(2) ||
		Address == reinterpret_cast<PROC>(3) || Address == reinterpret_cast<PROC>(-1))
	{
		throw ContextException(std::string("Required WGL procedure is unavailable: ") + Name);
	}
	return reinterpret_cast<Procedure>(Address);
}

struct OffscreenNativeHandles final
{
	HPBUFFERARB Pbuffer = nullptr;
	HDC DeviceContext = nullptr;
	HGLRC RenderingContext = nullptr;
	PFNWGLRELEASEPBUFFERDCARBPROC ReleaseDeviceContext = nullptr;
	PFNWGLDESTROYPBUFFERARBPROC DestroyPbuffer = nullptr;
	bool OwnsHandles = true;

	OffscreenNativeHandles() = default;
	OffscreenNativeHandles(const OffscreenNativeHandles &) = delete;
	OffscreenNativeHandles &operator=(const OffscreenNativeHandles &) = delete;
	OffscreenNativeHandles(OffscreenNativeHandles &&Other) noexcept
		: Pbuffer(std::exchange(Other.Pbuffer, nullptr)), DeviceContext(std::exchange(Other.DeviceContext, nullptr)),
		  RenderingContext(std::exchange(Other.RenderingContext, nullptr)),
		  ReleaseDeviceContext(std::exchange(Other.ReleaseDeviceContext, nullptr)),
		  DestroyPbuffer(std::exchange(Other.DestroyPbuffer, nullptr)), OwnsHandles(std::exchange(Other.OwnsHandles, false))
	{
	}
	~OffscreenNativeHandles()
	{
		if (!this->OwnsHandles)
			return;
		if (this->RenderingContext != nullptr)
			wglDeleteContext(this->RenderingContext);
		if (this->Pbuffer != nullptr && this->DeviceContext != nullptr && this->ReleaseDeviceContext != nullptr)
			this->ReleaseDeviceContext(this->Pbuffer, this->DeviceContext);
		if (this->Pbuffer != nullptr && this->DestroyPbuffer != nullptr)
			this->DestroyPbuffer(this->Pbuffer);
	}
	void Relinquish() noexcept
	{
		this->OwnsHandles = false;
	}
};

[[nodiscard]] OffscreenNativeHandles CreateOffscreenContextHandles()
{
	const HDC BootstrapDeviceContext = wglGetCurrentDC();
	const HGLRC BootstrapRenderingContext = wglGetCurrentContext();
	if (BootstrapDeviceContext == nullptr || BootstrapRenderingContext == nullptr)
		throw ContextException("WGL offscreen creation requires a current bootstrap context");

	const auto ChoosePixelFormat = RequireWGLProcedure<PFNWGLCHOOSEPIXELFORMATARBPROC>("wglChoosePixelFormatARB");
	const auto CreatePbuffer = RequireWGLProcedure<PFNWGLCREATEPBUFFERARBPROC>("wglCreatePbufferARB");
	const auto GetPbufferDeviceContext = RequireWGLProcedure<PFNWGLGETPBUFFERDCARBPROC>("wglGetPbufferDCARB");
	const auto ReleasePbufferDeviceContext = RequireWGLProcedure<PFNWGLRELEASEPBUFFERDCARBPROC>("wglReleasePbufferDCARB");
	const auto DestroyPbuffer = RequireWGLProcedure<PFNWGLDESTROYPBUFFERARBPROC>("wglDestroyPbufferARB");
	const auto CreateContext = RequireWGLProcedure<PFNWGLCREATECONTEXTATTRIBSARBPROC>("wglCreateContextAttribsARB");

	constexpr int32 PixelAttributes[]{WGL_DRAW_TO_PBUFFER_ARB,
									  TRUE,
									  WGL_SUPPORT_OPENGL_ARB,
									  TRUE,
									  WGL_ACCELERATION_ARB,
									  WGL_FULL_ACCELERATION_ARB,
									  WGL_PIXEL_TYPE_ARB,
									  WGL_TYPE_RGBA_ARB,
									  WGL_COLOR_BITS_ARB,
									  32,
									  WGL_DEPTH_BITS_ARB,
									  24,
									  WGL_STENCIL_BITS_ARB,
									  8,
									  WGL_DOUBLE_BUFFER_ARB,
									  FALSE,
									  0};
	int32 PixelFormat = 0;
	uint32 FormatCount = 0;
	if (ChoosePixelFormat(BootstrapDeviceContext, PixelAttributes, nullptr, 1, &PixelFormat, &FormatCount) == FALSE || FormatCount == 0)
	{
		throw ContextException("No hardware-accelerated WGL pbuffer pixel format satisfies the engine requirements");
	}

	HPBUFFERARB Pbuffer = CreatePbuffer(BootstrapDeviceContext, PixelFormat, 1, 1, nullptr);
	if (Pbuffer == nullptr)
		throw ContextException("WGL pbuffer creation failed with native error " + std::to_string(static_cast<uint32>(GetLastError())));
	HDC PbufferDeviceContext = GetPbufferDeviceContext(Pbuffer);
	if (PbufferDeviceContext == nullptr)
	{
		DestroyPbuffer(Pbuffer);
		throw ContextException("WGL pbuffer device-context acquisition failed");
	}

	constexpr int32 ContextAttributes[]{WGL_CONTEXT_MAJOR_VERSION_ARB,
										4,
										WGL_CONTEXT_MINOR_VERSION_ARB,
										6,
										WGL_CONTEXT_PROFILE_MASK_ARB,
										WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
										WGL_CONTEXT_FLAGS_ARB,
										WGL_CONTEXT_DEBUG_BIT_ARB | WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB |
											WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB,
										WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB,
										WGL_LOSE_CONTEXT_ON_RESET_ARB,
										0};
	HGLRC RenderingContext = CreateContext(PbufferDeviceContext, BootstrapRenderingContext, ContextAttributes);
	if (RenderingContext == nullptr)
	{
		ReleasePbufferDeviceContext(Pbuffer, PbufferDeviceContext);
		DestroyPbuffer(Pbuffer);
		throw ContextException("OpenGL 4.6 WGL pbuffer context creation failed with native error " +
							   std::to_string(static_cast<uint32>(GetLastError())));
	}

	OffscreenNativeHandles Handles;
	Handles.Pbuffer = Pbuffer;
	Handles.DeviceContext = PbufferDeviceContext;
	Handles.RenderingContext = RenderingContext;
	Handles.ReleaseDeviceContext = ReleasePbufferDeviceContext;
	Handles.DestroyPbuffer = DestroyPbuffer;
	return Handles;
}
} // namespace

WindowManager::WindowManager() : OwnerThread(std::this_thread::get_id())
{
	const HRESULT COMResult = OleInitialize(nullptr);
	if (FAILED(COMResult))
		throw WindowException("COM initialization failed with HRESULT " + std::to_string(static_cast<uint32>(COMResult)));
	this->COMInitialized = true;

	if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE)
	{
		const DWORD Error = GetLastError();
		if (Error != ERROR_ACCESS_DENIED)
		{
			OleUninitialize();
			this->COMInitialized = false;
			throw WindowException("Per-monitor DPI awareness initialization failed with native error " +
								  std::to_string(static_cast<uint32>(Error)));
		}
	}
	if (AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE)
	{
		OleUninitialize();
		this->COMInitialized = false;
		throw WindowException("The process is not configured for Per-Monitor DPI Awareness V2");
	}

	if (glfwInit() != GLFW_TRUE)
	{
		const std::string Diagnostic = GetGLFWDiagnostic("Window system initialization");
		OleUninitialize();
		this->COMInitialized = false;
		throw WindowException(Diagnostic);
	}
	this->GLFWInitialized = true;
}

WindowManager::~WindowManager()
{
	this->Shutdown();
}

Window &WindowManager::CreateWindow(const WindowSpecification &Specification)
{
	this->RequireOwnerThread();
	if (Specification.Title.empty())
		throw WindowException("Window title cannot be empty");
	if (!Specification.Extent.IsValid())
		throw WindowException("Window extent must be non-zero");
	if (Specification.ContextGroup.empty())
		throw WindowException("Window context-group name cannot be empty");

	ContextGroup &Group = this->GetOrCreateContextGroup(Specification.ContextGroup);
	this->ConfigureWindowHints(Specification);
	GLFWmonitor *Monitor = Specification.Mode == WindowMode::ExclusiveFullscreen ? glfwGetPrimaryMonitor() : nullptr;
	GLFWwindow *NativeWindow =
		glfwCreateWindow(static_cast<int32>(Specification.Extent.Width), static_cast<int32>(Specification.Extent.Height),
						 Specification.Title.c_str(), Monitor, Group.ShareRootWindow);
	if (NativeWindow == nullptr)
		throw WindowException(GetGLFWDiagnostic("Window creation"));

	const WindowID ID{this->NextWindowID++};
	std::unique_ptr<Window> WindowInstance;
	std::unique_ptr<Context> ContextInstance;
	bool ContextRegistered = false;
	bool ContextStored = false;
	try
	{
		WindowInstance = std::unique_ptr<Window>(new Window(*this, ID, Specification, NativeWindow));
		ContextInstance = std::unique_ptr<Context>(new Context(NativeWindow, Specification.ContextGroup, ID, false));
		ContextInstance->BindRenderThread();
		ContextInstance->MakeCurrent();
		WindowInstance->SetContext(*ContextInstance);
		if (Specification.Position)
			WindowInstance->SetPosition(*Specification.Position);
		if (Specification.Mode != WindowMode::Windowed)
			WindowInstance->SetMode(Specification.Mode);
		Group.Device->RegisterContext(*ContextInstance);
		ContextRegistered = true;
		Window *Result = WindowInstance.get();
		const auto [contextIterator, contextInserted] = Group.WindowContexts.emplace(ID, std::move(ContextInstance));
		if (!contextInserted)
			throw WindowException("Context group already contains the allocated WindowID");
		ContextStored = true;
		const auto [windowIterator, windowInserted] = this->Windows.emplace(ID, std::move(WindowInstance));
		if (!windowInserted)
			throw WindowException("WindowManager already contains the allocated WindowID");
		if (!this->PrimaryWindow.IsValid())
			this->PrimaryWindow = ID;
		return *Result;
	}
	catch (...)
	{
		if (ContextStored)
		{
			auto Stored = Group.WindowContexts.find(ID);
			if (Stored != Group.WindowContexts.end())
			{
				Group.Device->UnregisterContext(*Stored->second);
				Group.WindowContexts.erase(Stored);
			}
		}
		else if (ContextRegistered && ContextInstance != nullptr)
		{
			Group.Device->UnregisterContext(*ContextInstance);
		}
		ContextInstance.reset();
		if (WindowInstance != nullptr)
			WindowInstance.reset();
		else if (!this->Windows.contains(ID))
			glfwDestroyWindow(NativeWindow);
		if (!Group.AnchorContext->IsCurrent())
			Group.AnchorContext->MakeCurrent();
		throw;
	}
}

Window &WindowManager::RecreateWindow(const WindowID ID, const WindowSpecification &Specification)
{
	this->RequireOwnerThread();
	auto WindowIterator = this->Windows.find(ID);
	if (WindowIterator == this->Windows.end())
		throw WindowException("Cannot recreate unknown WindowID " + std::to_string(ID.Value));
	const std::string CurrentGroupName = WindowIterator->second->GetContext().GetShareGroupName();
	if (Specification.ContextGroup != CurrentGroupName)
		throw WindowException("Window recreation cannot change a Window's Context share group");
	if (Specification.Title.empty())
		throw WindowException("Window title cannot be empty");
	if (!Specification.Extent.IsValid())
		throw WindowException("Window extent must be non-zero");

	ContextGroup &Group = this->RequireContextGroup(CurrentGroupName);
	this->ConfigureWindowHints(Specification);
	GLFWmonitor *Monitor = Specification.Mode == WindowMode::ExclusiveFullscreen ? glfwGetPrimaryMonitor() : nullptr;
	GLFWwindow *NativeWindow =
		glfwCreateWindow(static_cast<int32>(Specification.Extent.Width), static_cast<int32>(Specification.Extent.Height),
						 Specification.Title.c_str(), Monitor, Group.ShareRootWindow);
	if (NativeWindow == nullptr)
		throw WindowException(GetGLFWDiagnostic("Window surface recreation"));

	std::unique_ptr<Window> ReplacementWindow;
	std::unique_ptr<Context> ReplacementContext;
	bool ReplacementRegistered = false;
	bool RecreationCommitted = false;
	try
	{
		ReplacementWindow = std::unique_ptr<Window>(new Window(*this, ID, Specification, NativeWindow));
		ReplacementContext = std::unique_ptr<Context>(new Context(NativeWindow, CurrentGroupName, ID, false));
		ReplacementContext->BindRenderThread();
		ReplacementContext->MakeCurrent();
		ReplacementWindow->SetContext(*ReplacementContext);
		if (Specification.Position)
			ReplacementWindow->SetPosition(*Specification.Position);
		if (Specification.Mode != WindowMode::Windowed)
			ReplacementWindow->SetMode(Specification.Mode);
		Group.Device->RegisterContext(*ReplacementContext);
		ReplacementRegistered = true;

		auto OldContextIterator = Group.WindowContexts.find(ID);
		if (OldContextIterator == Group.WindowContexts.end())
			throw WindowException("Window recreation found no owning Context");
		std::unique_ptr<Context> OldContext = std::move(OldContextIterator->second);
		OldContextIterator->second = std::move(ReplacementContext);
		WindowIterator->second->StateData.swap(ReplacementWindow->StateData);
		try
		{
			WindowIterator->second->RebindNativeRouting();
			ReplacementWindow->RebindNativeRouting();
		}
		catch (...)
		{
			WindowIterator->second->StateData.swap(ReplacementWindow->StateData);
			WindowIterator->second->RebindNativeRouting();
			ReplacementWindow->RebindNativeRouting();
			Group.Device->UnregisterContext(*OldContextIterator->second);
			ReplacementRegistered = false;
			ReplacementContext = std::move(OldContextIterator->second);
			OldContextIterator->second = std::move(OldContext);
			throw;
		}
		Group.Device->UnregisterContext(*OldContext);
		OldContext.reset();
		ReplacementWindow.reset();
		RecreationCommitted = true;
		this->EnqueueWindowEvent(
			{.Type = WindowEventType::FramebufferResized, .Window = ID, .Extent = WindowIterator->second->GetFramebufferExtent()});
		return *WindowIterator->second;
	}
	catch (...)
	{
		if (RecreationCommitted)
			throw;
		if (ReplacementRegistered && ReplacementContext != nullptr)
			Group.Device->UnregisterContext(*ReplacementContext);
		ReplacementContext.reset();
		if (ReplacementWindow != nullptr)
			ReplacementWindow.reset();
		else
			glfwDestroyWindow(NativeWindow);
		WindowIterator->second->GetContext().MakeCurrent();
		throw;
	}
}

void WindowManager::DestroyWindow(const WindowID ID)
{
	this->RequireOwnerThread();
	auto WindowIterator = this->Windows.find(ID);
	if (WindowIterator == this->Windows.end())
		throw WindowException("Cannot destroy unknown WindowID " + std::to_string(ID.Value));
	for (auto &[CandidateID, Candidate] : this->Windows)
	{
		(void)CandidateID;
		if (Candidate.get() == WindowIterator->second.get())
			continue;
		Candidate->DetachFromOwner(ID);
	}
	const std::string GroupName = WindowIterator->second->GetContext().GetShareGroupName();
	ContextGroup &Group = this->RequireContextGroup(GroupName);
	auto ContextIterator = Group.WindowContexts.find(ID);
	if (ContextIterator != Group.WindowContexts.end())
	{
		Group.Device->UnregisterContext(*ContextIterator->second);
		Group.WindowContexts.erase(ContextIterator);
		Group.AnchorContext->MakeCurrent();
	}
	this->Windows.erase(WindowIterator);
	this->EnqueueWindowEvent({.Type = WindowEventType::Closed, .Window = ID});
	if (this->PrimaryWindow == ID)
	{
		this->PrimaryWindow = this->Windows.empty() ? WindowID{} : this->Windows.begin()->first;
	}
}

Window *WindowManager::FindManagedWindow(const WindowID ID) noexcept
{
	const auto Iterator = this->Windows.find(ID);
	return Iterator == this->Windows.end() ? nullptr : Iterator->second.get();
}
const Window *WindowManager::FindManagedWindow(const WindowID ID) const noexcept
{
	const auto Iterator = this->Windows.find(ID);
	return Iterator == this->Windows.end() ? nullptr : Iterator->second.get();
}
Window &WindowManager::GetPrimaryWindow() const
{
	Window *Window = const_cast<WindowManager *>(this)->FindManagedWindow(this->PrimaryWindow);
	if (Window == nullptr)
		throw WindowException("WindowManager has no primary Window");
	return *Window;
}
void WindowManager::SetPrimaryWindow(const WindowID ID)
{
	this->RequireOwnerThread();
	if (this->FindManagedWindow(ID) == nullptr)
		throw WindowException("Cannot select an unknown primary Window");
	this->PrimaryWindow = ID;
}

std::vector<MonitorInfo> WindowManager::GetMonitors() const
{
	this->RequireOwnerThread();
	int32 MonitorCount = 0;
	GLFWmonitor **NativeMonitors = glfwGetMonitors(&MonitorCount);
	if (NativeMonitors == nullptr || MonitorCount <= 0)
		throw WindowException(GetGLFWDiagnostic("Monitor enumeration"));
	GLFWmonitor *Primary = glfwGetPrimaryMonitor();
	std::vector<MonitorInfo> Result;
	Result.reserve(static_cast<usize>(MonitorCount));
	for (int32 MonitorIndex = 0; MonitorIndex < MonitorCount; ++MonitorIndex)
	{
		GLFWmonitor *Monitor = NativeMonitors[MonitorIndex];
		const char *Identifier = glfwGetWin32Monitor(Monitor);
		const char *DisplayName = glfwGetMonitorName(Monitor);
		if (Identifier == nullptr)
			continue;
		MonitorInfo Info;
		Info.ID.Value = Identifier;
		Info.DisplayName = DisplayName == nullptr ? Info.ID.Value : DisplayName;
		int32 X = 0, Y = 0, Width = 0, Height = 0;
		glfwGetMonitorPos(Monitor, &X, &Y);
		Info.Position = {X, Y};
		glfwGetMonitorWorkarea(Monitor, &X, &Y, &Width, &Height);
		Info.WorkAreaPosition = {X, Y};
		Info.WorkAreaExtent = {static_cast<uint32>(std::max(Width, 0)), static_cast<uint32>(std::max(Height, 0))};
		glfwGetMonitorPhysicalSize(Monitor, &Width, &Height);
		Info.PhysicalWidthMillimeters = static_cast<uint32>(std::max(Width, 0));
		Info.PhysicalHeightMillimeters = static_cast<uint32>(std::max(Height, 0));
		glfwGetMonitorContentScale(Monitor, &Info.ContentScaleX, &Info.ContentScaleY);
		const GLFWvidmode *Current = glfwGetVideoMode(Monitor);
		if (Current == nullptr)
			throw WindowException(GetGLFWDiagnostic("Current monitor mode query"));
		Info.CurrentMode = {.Extent = {static_cast<uint32>(Current->width), static_cast<uint32>(Current->height)},
							.RedBits = static_cast<uint32>(Current->redBits),
							.GreenBits = static_cast<uint32>(Current->greenBits),
							.BlueBits = static_cast<uint32>(Current->blueBits),
							.RefreshRate = static_cast<uint32>(Current->refreshRate)};
		int32 ModeCount = 0;
		const GLFWvidmode *Modes = glfwGetVideoModes(Monitor, &ModeCount);
		if (Modes == nullptr || ModeCount <= 0)
			throw WindowException(GetGLFWDiagnostic("Monitor video-mode query"));
		Info.Modes.reserve(static_cast<usize>(ModeCount));
		for (int32 ModeIndex = 0; ModeIndex < ModeCount; ++ModeIndex)
		{
			const GLFWvidmode &Mode = Modes[ModeIndex];
			Info.Modes.push_back({.Extent = {static_cast<uint32>(Mode.width), static_cast<uint32>(Mode.height)},
								  .RedBits = static_cast<uint32>(Mode.redBits),
								  .GreenBits = static_cast<uint32>(Mode.greenBits),
								  .BlueBits = static_cast<uint32>(Mode.blueBits),
								  .RefreshRate = static_cast<uint32>(Mode.refreshRate)});
		}
		Info.Primary = Monitor == Primary;
		Result.push_back(std::move(Info));
	}
	if (Result.empty())
		throw WindowException("Monitor enumeration returned no usable monitors");
	return Result;
}

MonitorInfo WindowManager::GetPrimaryMonitor() const
{
	std::vector<MonitorInfo> Monitors = this->GetMonitors();
	const auto Primary = std::find_if(Monitors.begin(), Monitors.end(), [](const MonitorInfo &Monitor) { return Monitor.Primary; });
	if (Primary == Monitors.end())
		throw WindowException("Window system did not identify a primary monitor");
	return *Primary;
}
pipeline::device::Device &WindowManager::GetDevice(const std::string_view ContextGroup) const
{
	return *this->RequireContextGroup(ContextGroup).Device;
}
pipeline::device::Device &WindowManager::GetDevice(const Window &Window) const
{
	return this->GetDevice(Window.GetContext().GetShareGroupName());
}
Context &WindowManager::GetAnchorContext(const std::string_view ContextGroup) const
{
	return *this->RequireContextGroup(ContextGroup).AnchorContext;
}

void WindowManager::PollEvents()
{
	this->RequireOwnerThread();
	glfwPollEvents();
	this->RethrowNativeException();
	this->WindowEventDispatcher.RethrowPendingException();
	this->InputEventDispatcher.RethrowPendingException();
}
void WindowManager::WaitEvents(const float64 TimeoutSeconds)
{
	this->RequireOwnerThread();
	if (TimeoutSeconds < 0.0)
		throw WindowException("Event wait timeout cannot be negative");
	glfwWaitEventsTimeout(TimeoutSeconds);
	this->RethrowNativeException();
	this->WindowEventDispatcher.RethrowPendingException();
	this->InputEventDispatcher.RethrowPendingException();
}
void WindowManager::WakeEventLoop()
{
	glfwPostEmptyEvent();
}

std::vector<WindowEvent> WindowManager::ConsumeEvents()
{
	this->RequireOwnerThread();
	std::vector<WindowEvent> Result;
	Result.reserve(this->Events.size());
	while (!this->Events.empty())
	{
		Result.push_back(std::move(this->Events.front()));
		this->Events.pop_front();
	}
	return Result;
}

std::vector<input::InputEvent> WindowManager::ConsumeInputEvents()
{
	this->RequireOwnerThread();
	std::vector<input::InputEvent> Result;
	Result.reserve(this->InputEvents.size());
	while (!this->InputEvents.empty())
	{
		Result.push_back(std::move(this->InputEvents.front()));
		this->InputEvents.pop_front();
	}
	return Result;
}

EventSubscription WindowManager::SubscribeWindowEvents(const int32 Priority, EventDispatcher<WindowEvent>::Callback Callback)
{
	this->RequireOwnerThread();
	return this->WindowEventDispatcher.Subscribe(Priority, std::move(Callback));
}

EventSubscription WindowManager::SubscribeInputEvents(const int32 Priority, EventDispatcher<input::InputEvent>::Callback Callback)
{
	this->RequireOwnerThread();
	return this->InputEventDispatcher.Subscribe(Priority, std::move(Callback));
}

bool WindowManager::IsOwnerThread() const noexcept
{
	return this->OwnerThread == std::this_thread::get_id();
}
void WindowManager::RequireOwnerThread() const
{
	if (!this->IsOwnerThread())
		throw WindowException("WindowManager operation executed outside its owner thread");
}

WindowManager::ContextGroup &WindowManager::GetOrCreateContextGroup(const std::string &Name)
{
	const auto Existing = this->ContextGroups.find(Name);
	if (Existing != this->ContextGroups.end())
		return *Existing->second;
	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
	glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
	glfwWindowHint(GLFW_CONTEXT_ROBUSTNESS, GLFW_LOSE_CONTEXT_ON_RESET);
	glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow *ShareRootWindow = glfwCreateWindow(1, 1, "Context Share Root", nullptr, nullptr);
	if (ShareRootWindow == nullptr)
		throw WindowException(GetGLFWDiagnostic("Context-group share-root creation"));
	try
	{
		glfwMakeContextCurrent(ShareRootWindow);
		OffscreenNativeHandles NativeHandles = CreateOffscreenContextHandles();
		Context::OffscreenHandles OffscreenHandles{.Pbuffer = static_cast<void *>(NativeHandles.Pbuffer),
												   .DeviceContext = static_cast<void *>(NativeHandles.DeviceContext),
												   .RenderingContext = static_cast<void *>(NativeHandles.RenderingContext),
												   .ReleaseDeviceContext = reinterpret_cast<void *>(NativeHandles.ReleaseDeviceContext),
												   .DestroyPbuffer = reinterpret_cast<void *>(NativeHandles.DestroyPbuffer)};
		auto Group = std::make_unique<ContextGroup>();
		Group->Name = Name;
		Group->ShareRootWindow = ShareRootWindow;
		Group->AnchorContext = std::unique_ptr<Context>(new Context(OffscreenHandles, Name));
		NativeHandles.Relinquish();
		Group->AnchorContext->BindRenderThread();
		Group->AnchorContext->MakeCurrent();
		Group->Device = std::make_unique<pipeline::device::Device>(*Group->AnchorContext);
		ContextGroup &Result = *Group;
		this->ContextGroups.emplace(Name, std::move(Group));
		return Result;
	}
	catch (...)
	{
		glfwDestroyWindow(ShareRootWindow);
		throw;
	}
}

WindowManager::ContextGroup &WindowManager::RequireContextGroup(const std::string_view Name) const
{
	const auto Iterator = this->ContextGroups.find(std::string(Name));
	if (Iterator == this->ContextGroups.end())
		throw WindowException("Unknown Context group '" + std::string(Name) + "'");
	return *Iterator->second;
}

void *WindowManager::FindNativeMonitor(const MonitorID &ID) const
{
	this->RequireOwnerThread();
	if (!ID.IsValid())
		throw WindowException("MonitorID cannot be empty");
	int32 Count = 0;
	GLFWmonitor **Monitors = glfwGetMonitors(&Count);
	for (int32 Index = 0; Monitors != nullptr && Index < Count; ++Index)
	{
		const char *Identifier = glfwGetWin32Monitor(Monitors[Index]);
		if (Identifier != nullptr && ID.Value == Identifier)
			return Monitors[Index];
	}
	throw WindowException("Unknown MonitorID '" + ID.Value + "'");
}

void WindowManager::EnqueueWindowEvent(WindowEvent Event)
{
	this->RequireOwnerThread();
	if (this->WindowEventDispatcher.Dispatch(Event) == EventPropagation::Consumed)
		return;
	if (IsReplaceableEvent(Event.Type))
	{
		const auto Existing = std::find_if(this->Events.rbegin(), this->Events.rend(), [&Event](const WindowEvent &Candidate)
										   { return Candidate.Type == Event.Type && Candidate.Window == Event.Window; });
		if (Existing != this->Events.rend())
		{
			*Existing = std::move(Event);
			return;
		}
	}
	this->Events.push_back(std::move(Event));
}

void WindowManager::EnqueueInputEvent(input::InputEvent Event)
{
	this->RequireOwnerThread();
	(void)this->InputEventDispatcher.Dispatch(Event);
	if (Event.Type == input::InputEventType::MouseMove || Event.Type == input::InputEventType::ControllerState)
	{
		const auto Existing = std::find_if(
			this->InputEvents.rbegin(), this->InputEvents.rend(),
			[&Event](const input::InputEvent &Candidate)
			{
				return Candidate.Type == Event.Type && Candidate.Window == Event.Window && Candidate.User == Event.User &&
					   (Event.Type != input::InputEventType::ControllerState || Candidate.ControllerState.ID == Event.ControllerState.ID);
			});
		if (Existing != this->InputEvents.rend())
		{
			*Existing = std::move(Event);
			return;
		}
	}
	this->InputEvents.push_back(std::move(Event));
}

void WindowManager::RecordNativeException(std::exception_ptr Exception) noexcept
{
	if (this->PendingNativeException == nullptr)
		this->PendingNativeException = std::move(Exception);
}

void WindowManager::RethrowNativeException()
{
	std::exception_ptr Pending = std::exchange(this->PendingNativeException, nullptr);
	if (Pending != nullptr)
		std::rethrow_exception(Pending);
}

void WindowManager::ConfigureWindowHints(const WindowSpecification &Specification) const
{
	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
	glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
	glfwWindowHint(GLFW_CONTEXT_ROBUSTNESS, GLFW_LOSE_CONTEXT_ON_RESET);
	glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, Specification.Visible && !Specification.HeadlessValidation ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint(GLFW_FOCUSED, Specification.Focused ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint(GLFW_DECORATED, Specification.Decorated ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint(GLFW_RESIZABLE, Specification.Resizable ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint(GLFW_FLOATING, Specification.Floating ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, Specification.TransparentFramebuffer ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
}

void WindowManager::Shutdown() noexcept
{
	if (!this->IsOwnerThread())
		std::terminate();
	for (auto &[name, group] : this->ContextGroups)
	{
		for (auto &[id, context] : group->WindowContexts)
			group->Device->UnregisterContext(*context);
		group->WindowContexts.clear();
	}
	this->Windows.clear();
	for (auto &[name, group] : this->ContextGroups)
	{
		if (group->AnchorContext && !group->AnchorContext->IsCurrent())
		{
			try
			{
				group->AnchorContext->MakeCurrent();
			}
			catch (...)
			{
			}
		}
		group->Device.reset();
		group->AnchorContext.reset();
		if (group->ShareRootWindow != nullptr)
			glfwDestroyWindow(group->ShareRootWindow);
		group->ShareRootWindow = nullptr;
	}
	this->ContextGroups.clear();
	this->Events.clear();
	this->InputEvents.clear();
	if (this->GLFWInitialized)
	{
		glfwTerminate();
		this->GLFWInitialized = false;
	}
	if (this->COMInitialized)
	{
		OleUninitialize();
		this->COMInitialized = false;
	}
}
} // namespace core
