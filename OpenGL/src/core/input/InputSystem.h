#pragma once

#include "InputTypes.h"

#include <array>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <vector>

namespace core
{
class WindowManager;
}

namespace core::input
{
class InputSystem final
{
  public:
	InputSystem() = default;
	~InputSystem();

	InputSystem(const InputSystem &) = delete;
	InputSystem &operator=(const InputSystem &) = delete;
	InputSystem(InputSystem &&) = delete;
	InputSystem &operator=(InputSystem &&) = delete;

	void BeginFrame(WindowManager &WindowManager);
	[[nodiscard]] const InputSnapshot &GetSnapshot(WindowID Window, LocalUserID User = {}) const;
	void DefineAction(InputAction Action);
	void RemoveAction(ActionID Action);
	void CreateContext(InputContextID ID, int32 Priority);
	void RemoveContext(InputContextID ID);
	void SetContextEnabled(InputContextID ID, bool Enabled);
	void AddBinding(InputContextID Context, InputBinding Binding);
	void AddComposite(InputContextID Context, InputComposite Composite);
	void Rebind(InputContextID Context, ActionID Action, InputBinding Replacement);
	void RemoveBinding(InputContextID Context, ActionID Action);
	[[nodiscard]] ActionState GetAction(ActionID Action, WindowID Window, LocalUserID User = {}) const;
	[[nodiscard]] ControllerCapabilities GetControllerCapabilities(ControllerID Controller) const;
	void SetControllerHaptics(ControllerID Controller, HapticState State);
	void StopAllHaptics() noexcept;

  private:
	struct SnapshotKey final
	{
		WindowID Window;
		LocalUserID User;
		auto operator<=>(const SnapshotKey &) const = default;
	};

	struct SnapshotKeyHash final
	{
		usize operator()(const SnapshotKey &Key) const noexcept;
	};

	struct ActionContext final
	{
		InputContextID ID = 0;
		int32 Priority = 0;
		bool Enabled = true;
		std::vector<InputBinding> Bindings;
	};

	[[nodiscard]] static bool Conflicts(const InputBinding &Left, const InputBinding &Right) noexcept;
	[[nodiscard]] static float32 Evaluate(const InputBinding &Binding, const InputSnapshot &Snapshot);
	[[nodiscard]] static bool IsTriggered(const InputBinding &Binding, const InputSnapshot &Snapshot);
	[[nodiscard]] static bool IsReleased(const InputBinding &Binding, const InputSnapshot &Snapshot);
	void ValidateBinding(const InputBinding &Binding) const;
	[[nodiscard]] ActionContext &RequireContext(InputContextID ID);
	[[nodiscard]] const ActionContext &RequireContext(InputContextID ID) const;
	void PollControllers(WindowManager &WindowManager);

	std::unordered_map<SnapshotKey, InputSnapshot, SnapshotKeyHash> Snapshots;
	std::unordered_map<ActionID, InputAction> Actions;
	std::vector<ActionContext> Contexts;
	std::array<std::optional<ControllerCapabilities>, 4> ControllerCapabilitySlots;
	std::array<uint32, 4> ControllerPackets{};
	std::array<std::chrono::steady_clock::time_point, 4> ControllerCapabilityRefresh{};
};
} // namespace core::input
