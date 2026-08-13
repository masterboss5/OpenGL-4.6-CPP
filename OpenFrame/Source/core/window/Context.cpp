#include "Context.h"

#include <atomic>
#include <exception>
#include <mutex>
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
	mutable std::mutex OwnershipMutex;
	std::thread::id RenderThread;
	std::atomic<ContextStatus> Status = ContextStatus::Ready;
	bool Offscreen = false;
	bool OwnsWindowDeviceContext = false;
	bool ThreadTransferPending = false;
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
	std::scoped_lock Lock(this->State->OwnershipMutex);
	if (this->State->ThreadTransferPending ||
		(this->State->RenderThread != std::thread::id{} && this->State->RenderThread != std::this_thread::get_id()))
	{
		std::terminate();
	}
	this->State->Status.store(ContextStatus::Lost, std::memory_order_release);
	if (wglGetCurrentContext() == this->State->RenderingContext)
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
	std::scoped_lock Lock(this->State->OwnershipMutex);
	if (this->State->Status.load(std::memory_order_acquire) != ContextStatus::Ready)
		throw ContextException("Cannot bind a reset or lost Context");
	if (this->State->ThreadTransferPending)
		throw ContextException("Cannot bind a Context while its thread transfer is pending");
	if (this->State->RenderThread != std::thread::id{} && this->State->RenderThread != std::this_thread::get_id())
		throw ContextException("Context may only be made current on its assigned render thread");
	if (wglMakeCurrent(this->State->DeviceContext, this->State->RenderingContext) == FALSE)
		throw ContextException(this->State->Offscreen ? "Failed to make offscreen Context current"
													  : "Failed to make visible Context current");
}

void Context::ReleaseCurrent()
{
	std::scoped_lock Lock(this->State->OwnershipMutex);
	if (wglGetCurrentContext() != this->State->RenderingContext)
		throw ContextException("Cannot release a Context that is not current on this thread");
	if (wglMakeCurrent(nullptr, nullptr) == FALSE)
		throw ContextException("Failed to release the current Context");
}

void Context::BindRenderThread()
{
	std::scoped_lock Lock(this->State->OwnershipMutex);
	const std::thread::id Current = std::this_thread::get_id();
	if (this->State->ThreadTransferPending)
		throw ContextException("Context has a pending thread transfer; adopt it instead of binding it");
	if (this->State->RenderThread != std::thread::id{} && this->State->RenderThread != Current)
		throw ContextException("Context render-thread affinity is already assigned");
	this->State->RenderThread = Current;
}

void Context::PrepareThreadTransfer()
{
	std::scoped_lock Lock(this->State->OwnershipMutex);
	const std::thread::id Current = std::this_thread::get_id();
	if (this->State->ThreadTransferPending)
		throw ContextException("Context already has a pending thread transfer");
	if (this->State->RenderThread != Current)
		throw ContextException("Only the assigned render thread may transfer a Context");
	if (wglGetCurrentContext() == this->State->RenderingContext && wglMakeCurrent(nullptr, nullptr) == FALSE)
		throw ContextException("Failed to release the Context for thread transfer");
	this->State->RenderThread = {};
	this->State->ThreadTransferPending = true;
}

void Context::AdoptCurrentThread()
{
	std::scoped_lock Lock(this->State->OwnershipMutex);
	if (!this->State->ThreadTransferPending)
		throw ContextException("Context has not been prepared for a thread transfer");
	if (this->State->RenderThread != std::thread::id{})
		throw ContextException("Context thread transfer cannot replace an assigned render thread");

	this->State->RenderThread = std::this_thread::get_id();
	try
	{
		if (this->State->Status.load(std::memory_order_acquire) != ContextStatus::Ready)
			throw ContextException("Cannot adopt a reset or lost Context");
		if (wglMakeCurrent(this->State->DeviceContext, this->State->RenderingContext) == FALSE)
			throw ContextException("Failed to make the transferred Context current");
		this->State->ThreadTransferPending = false;
	}
	catch (...)
	{
		this->State->RenderThread = {};
		throw;
	}
}

void Context::RequireCurrentThread() const
{
	std::scoped_lock Lock(this->State->OwnershipMutex);
	if (this->State->Status.load(std::memory_order_acquire) != ContextStatus::Ready)
		throw ContextException("Context is reset or lost");
	if (this->State->RenderThread != std::thread::id{} && this->State->RenderThread != std::this_thread::get_id())
		throw ContextException("OpenGL operation executed outside the Context render thread");
	if (wglGetCurrentContext() != this->State->RenderingContext)
		throw ContextException("OpenGL operation requires the owning Context to be current");
}

bool Context::IsCurrent() const noexcept
{
	return wglGetCurrentContext() == this->State->RenderingContext;
}
bool Context::IsThreadTransferPending() const noexcept
{
	std::scoped_lock Lock(this->State->OwnershipMutex);
	return this->State->ThreadTransferPending;
}
bool Context::IsOffscreen() const noexcept
{
	return this->State->Offscreen;
}
ContextStatus Context::GetStatus() const noexcept
{
	return this->State->Status.load(std::memory_order_acquire);
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
	this->State->Status.store(ContextStatus::Reset, std::memory_order_release);
}
void *Context::GetNativeWindow() const noexcept
{
	return this->State->NativeWindow;
}
} // namespace core
