#pragma once

#include "WindowTypes.h"

#include <memory>
#include <string>
#include <thread>

namespace core
{
class Window;
class WindowManager;

enum class ContextStatus : uint8
{
	Ready,
	Reset,
	Lost
};

class Context final
{
  public:
	~Context();

	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;
	Context(Context &&) = delete;
	Context &operator=(Context &&) = delete;

	void MakeCurrent();
	void ReleaseCurrent();
	void BindRenderThread();
	void RequireCurrentThread() const;
	[[nodiscard]] bool IsCurrent() const noexcept;
	[[nodiscard]] bool IsOffscreen() const noexcept;
	[[nodiscard]] ContextStatus GetStatus() const noexcept;
	[[nodiscard]] const std::string &GetShareGroupName() const noexcept;
	[[nodiscard]] WindowID GetWindowID() const noexcept;
	void MarkReset() noexcept;

  private:
	struct StateData;
	struct OffscreenHandles final
	{
		void *Pbuffer = nullptr;
		void *DeviceContext = nullptr;
		void *RenderingContext = nullptr;
		void *ReleaseDeviceContext = nullptr;
		void *DestroyPbuffer = nullptr;
	};
	friend class WindowManager;

	Context(void *NativeWindow, std::string ShareGroupName, WindowID WindowID, bool Offscreen);
	Context(OffscreenHandles Handles, std::string ShareGroupName);
	[[nodiscard]] void *GetNativeWindow() const noexcept;

	std::unique_ptr<StateData> State;
};
} // namespace core
