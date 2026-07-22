#pragma once

#include "Window.h"
#include "src/core/events/EventDispatcher.h"
#include "src/core/input/InputTypes.h"

#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pipeline::device
{
class Device;
}

namespace core::input
{
class InputSystem;
}

namespace core
{
class Context;

class WindowManager final
{
  public:
	WindowManager();
	~WindowManager();

	WindowManager(const WindowManager &) = delete;
	WindowManager &operator=(const WindowManager &) = delete;
	WindowManager(WindowManager &&) = delete;
	WindowManager &operator=(WindowManager &&) = delete;

	[[nodiscard]] Window &CreateWindow(const WindowSpecification &Specification);
	[[nodiscard]] Window &RecreateWindow(WindowID ID, const WindowSpecification &Specification);
	void DestroyWindow(WindowID ID);
	[[nodiscard]] Window *FindManagedWindow(WindowID ID) noexcept;
	[[nodiscard]] const Window *FindManagedWindow(WindowID ID) const noexcept;
	[[nodiscard]] Window &GetPrimaryWindow() const;
	void SetPrimaryWindow(WindowID ID);
	[[nodiscard]] std::vector<MonitorInfo> GetMonitors() const;
	[[nodiscard]] MonitorInfo GetPrimaryMonitor() const;
	[[nodiscard]] pipeline::device::Device &GetDevice(std::string_view ContextGroup) const;
	[[nodiscard]] pipeline::device::Device &GetDevice(const Window &Window) const;
	[[nodiscard]] Context &GetAnchorContext(std::string_view ContextGroup) const;
	void PollEvents();
	void WaitEvents(float64 TimeoutSeconds);
	void WakeEventLoop();
	[[nodiscard]] std::vector<WindowEvent> ConsumeEvents();
	[[nodiscard]] std::vector<input::InputEvent> ConsumeInputEvents();
	[[nodiscard]] EventSubscription SubscribeWindowEvents(int32 Priority, EventDispatcher<WindowEvent>::Callback Callback);
	[[nodiscard]] EventSubscription SubscribeInputEvents(int32 Priority, EventDispatcher<input::InputEvent>::Callback Callback);
	[[nodiscard]] bool IsOwnerThread() const noexcept;
	void RequireOwnerThread() const;

  private:
	struct ContextGroup;
	friend class Window;
	friend struct WindowCallbacks;
	friend class input::InputSystem;

	[[nodiscard]] ContextGroup &GetOrCreateContextGroup(const std::string &Name);
	[[nodiscard]] ContextGroup &RequireContextGroup(std::string_view Name) const;
	[[nodiscard]] void *FindNativeMonitor(const MonitorID &ID) const;
	void EnqueueWindowEvent(WindowEvent Event);
	void EnqueueInputEvent(input::InputEvent Event);
	void RecordNativeException(std::exception_ptr Exception) noexcept;
	void RethrowNativeException();
	void ConfigureWindowHints(const WindowSpecification &Specification) const;
	void Shutdown() noexcept;

	std::thread::id OwnerThread;
	std::unordered_map<WindowID, std::unique_ptr<Window>> Windows;
	std::unordered_map<std::string, std::unique_ptr<ContextGroup>> ContextGroups;
	std::deque<WindowEvent> Events;
	std::deque<input::InputEvent> InputEvents;
	EventDispatcher<WindowEvent> WindowEventDispatcher;
	EventDispatcher<input::InputEvent> InputEventDispatcher;
	WindowID PrimaryWindow;
	uint64 NextWindowID = 1;
	bool GLFWInitialized = false;
	bool COMInitialized = false;
	std::exception_ptr PendingNativeException;
};
} // namespace core
