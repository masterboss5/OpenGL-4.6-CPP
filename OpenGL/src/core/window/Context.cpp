#include "Context.h"

#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <GL/glew.h>
#include <GL/wglew.h>
#include <Windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WGL
#include "WindowException.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace core
{
struct Context::StateData final
{
	GLFWwindow *NativeWindow = nullptr;
	HPBUFFERARB Pbuffer = nullptr;
	HDC DeviceContext = nullptr;
	HGLRC RenderingContext = nullptr;
	PFNWGLRELEASEPBUFFERDCARBPROC ReleaseDeviceContext = nullptr;
	PFNWGLDESTROYPBUFFERARBPROC DestroyPbuffer = nullptr;
	std::string ShareGroupName;
	WindowID WindowID;
	std::thread::id RenderThread;
	ContextStatus Status = ContextStatus::Ready;
	bool Offscreen = false;
	bool OwnsWindowDeviceContext = false;
};

Context::Context(void *NativeWindow, std::string ShareGroupName, const WindowID WindowID, const bool Offscreen)
	: State(std::make_unique<Context::StateData>())
{
	if (NativeWindow == nullptr)
		throw ContextException("Cannot construct Context without a native context surface");
	if (ShareGroupName.empty())
		throw ContextException("Context share-group name cannot be empty");
	this->State->NativeWindow = static_cast<GLFWwindow *>(NativeWindow);
	HWND SystemWindow = glfwGetWin32Window(this->State->NativeWindow);
	if (SystemWindow == nullptr)
		throw ContextException("Visible window does not expose a valid system window");
	this->State->DeviceContext = GetDC(SystemWindow);
	if (this->State->DeviceContext == nullptr)
		throw ContextException("Visible window device-context acquisition failed");
	this->State->OwnsWindowDeviceContext = true;
	this->State->RenderingContext = glfwGetWGLContext(this->State->NativeWindow);
	if (this->State->RenderingContext == nullptr)
	{
		ReleaseDC(SystemWindow, this->State->DeviceContext);
		this->State->DeviceContext = nullptr;
		this->State->OwnsWindowDeviceContext = false;
		throw ContextException("Visible window does not own a valid WGL rendering context");
	}
	this->State->ShareGroupName = std::move(ShareGroupName);
	this->State->WindowID = WindowID;
	this->State->Offscreen = Offscreen;
}

Context::Context(OffscreenHandles Handles, std::string ShareGroupName) : State(std::make_unique<Context::StateData>())
{
	if (Handles.Pbuffer == nullptr || Handles.DeviceContext == nullptr || Handles.RenderingContext == nullptr ||
		Handles.ReleaseDeviceContext == nullptr || Handles.DestroyPbuffer == nullptr)
	{
		throw ContextException("Cannot construct an offscreen Context from incomplete WGL handles");
	}
	if (ShareGroupName.empty())
		throw ContextException("Context share-group name cannot be empty");
	this->State->Pbuffer = static_cast<HPBUFFERARB>(Handles.Pbuffer);
	this->State->DeviceContext = static_cast<HDC>(Handles.DeviceContext);
	this->State->RenderingContext = static_cast<HGLRC>(Handles.RenderingContext);
	this->State->ReleaseDeviceContext = reinterpret_cast<PFNWGLRELEASEPBUFFERDCARBPROC>(Handles.ReleaseDeviceContext);
	this->State->DestroyPbuffer = reinterpret_cast<PFNWGLDESTROYPBUFFERARBPROC>(Handles.DestroyPbuffer);
	this->State->ShareGroupName = std::move(ShareGroupName);
	this->State->Offscreen = true;
}

Context::~Context()
{
	if (this->IsCurrent())
		wglMakeCurrent(nullptr, nullptr);
	if (this->State->Offscreen)
	{
		if (this->State->RenderingContext != nullptr)
			wglDeleteContext(this->State->RenderingContext);
		if (this->State->Pbuffer != nullptr && this->State->DeviceContext != nullptr)
			this->State->ReleaseDeviceContext(this->State->Pbuffer, this->State->DeviceContext);
		if (this->State->Pbuffer != nullptr)
			this->State->DestroyPbuffer(this->State->Pbuffer);
		this->State->RenderingContext = nullptr;
		this->State->DeviceContext = nullptr;
		this->State->Pbuffer = nullptr;
	}
	else if (this->State->OwnsWindowDeviceContext && this->State->DeviceContext != nullptr && this->State->NativeWindow != nullptr)
	{
		HWND SystemWindow = glfwGetWin32Window(this->State->NativeWindow);
		if (SystemWindow != nullptr)
			ReleaseDC(SystemWindow, this->State->DeviceContext);
		this->State->DeviceContext = nullptr;
		this->State->RenderingContext = nullptr;
		this->State->OwnsWindowDeviceContext = false;
	}
	this->State->NativeWindow = nullptr;
}

void Context::MakeCurrent()
{
	if (this->State->Status != ContextStatus::Ready)
		throw ContextException("Cannot bind a reset or lost Context");
	if (this->State->RenderThread != std::thread::id{} && this->State->RenderThread != std::this_thread::get_id())
		throw ContextException("Context may only be made current on its assigned render thread");
	if (wglMakeCurrent(this->State->DeviceContext, this->State->RenderingContext) == FALSE)
		throw ContextException(this->State->Offscreen ? "Failed to make offscreen Context current"
													  : "Failed to make visible Context current");
}

void Context::ReleaseCurrent()
{
	if (!this->IsCurrent())
		throw ContextException("Cannot release a Context that is not current on this thread");
	wglMakeCurrent(nullptr, nullptr);
}

void Context::BindRenderThread()
{
	const std::thread::id Current = std::this_thread::get_id();
	if (this->State->RenderThread != std::thread::id{} && this->State->RenderThread != Current)
		throw ContextException("Context render-thread affinity is already assigned");
	this->State->RenderThread = Current;
}

void Context::RequireCurrentThread() const
{
	if (this->State->Status != ContextStatus::Ready)
		throw ContextException("Context is reset or lost");
	if (this->State->RenderThread != std::thread::id{} && this->State->RenderThread != std::this_thread::get_id())
		throw ContextException("OpenGL operation executed outside the Context render thread");
	if (!this->IsCurrent())
		throw ContextException("OpenGL operation requires the owning Context to be current");
}

bool Context::IsCurrent() const noexcept
{
	return wglGetCurrentContext() == this->State->RenderingContext;
}
bool Context::IsOffscreen() const noexcept
{
	return this->State->Offscreen;
}
ContextStatus Context::GetStatus() const noexcept
{
	return this->State->Status;
}
const std::string &Context::GetShareGroupName() const noexcept
{
	return this->State->ShareGroupName;
}
WindowID Context::GetWindowID() const noexcept
{
	return this->State->WindowID;
}
void Context::MarkReset() noexcept
{
	this->State->Status = ContextStatus::Reset;
}
void *Context::GetNativeWindow() const noexcept
{
	return this->State->NativeWindow;
}
} // namespace core
