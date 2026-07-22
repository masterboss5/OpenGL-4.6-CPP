#pragma once

#include "src/core/window/WindowTypes.h"
#include "src/types.h"

#include <array>
#include <compare>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace core::input
{
struct LocalUserID final
{
	uint8 Value = 0;
	auto operator<=>(const LocalUserID &) const = default;
};

struct ControllerID final
{
	uint8 Value = 0;
	auto operator<=>(const ControllerID &) const = default;
};

struct PhysicalKey final
{
	uint16 ScanCode = 0;
	bool Extended = false;
	auto operator<=>(const PhysicalKey &) const = default;
};

enum class Key : uint16
{
	Unknown,
	Space,
	Apostrophe,
	Comma,
	Minus,
	Period,
	Slash,
	Number0,
	Number1,
	Number2,
	Number3,
	Number4,
	Number5,
	Number6,
	Number7,
	Number8,
	Number9,
	Semicolon,
	Equal,
	A,
	B,
	C,
	D,
	E,
	F,
	G,
	H,
	I,
	J,
	K,
	L,
	M,
	N,
	O,
	P,
	Q,
	R,
	S,
	T,
	U,
	V,
	W,
	X,
	Y,
	Z,
	Escape,
	Enter,
	Tab,
	Backspace,
	Insert,
	Delete,
	Right,
	Left,
	Down,
	Up,
	PageUp,
	PageDown,
	Home,
	End,
	CapsLock,
	ScrollLock,
	NumLock,
	PrintScreen,
	Pause,
	F1,
	F2,
	F3,
	F4,
	F5,
	F6,
	F7,
	F8,
	F9,
	F10,
	F11,
	F12,
	LeftShift,
	LeftControl,
	LeftAlt,
	LeftSuper,
	RightShift,
	RightControl,
	RightAlt,
	RightSuper,
	Menu,
	Count
};

enum class MouseButton : uint8
{
	Left,
	Right,
	Middle,
	Button4,
	Button5,
	Button6,
	Button7,
	Button8,
	Count
};

enum class InputState : uint8
{
	Released,
	Pressed,
	Repeated
};

enum class InputEventType : uint8
{
	Key,
	Text,
	MouseButton,
	MouseMove,
	MouseScroll,
	CursorEntered,
	FocusLost,
	IMEComposition,
	Touch,
	Pen,
	Gesture,
	ControllerConnected,
	ControllerDisconnected,
	ControllerCapabilitiesChanged,
	ControllerState
};

enum class ContactPhase : uint8
{
	Began,
	Moved,
	Ended,
	Cancelled
};
enum class GestureType : uint8
{
	Pan,
	Zoom,
	Rotate,
	TwoFingerTap,
	PressAndTap
};

struct TouchContact final
{
	uint32 ID = 0;
	ContactPhase Phase = ContactPhase::Moved;
	float64 X = 0.0;
	float64 Y = 0.0;
	float32 Pressure = 0.0f;
	WindowExtent ContactExtent;
	bool Primary = false;
};

struct PenContact final
{
	uint32 ID = 0;
	ContactPhase Phase = ContactPhase::Moved;
	float64 X = 0.0;
	float64 Y = 0.0;
	float32 Pressure = 0.0f;
	float32 TiltX = 0.0f;
	float32 TiltY = 0.0f;
	float32 Rotation = 0.0f;
	bool Eraser = false;
	bool Primary = false;
};

struct Gesture final
{
	GestureType Type = GestureType::Pan;
	float64 X = 0.0;
	float64 Y = 0.0;
	float64 DeltaX = 0.0;
	float64 DeltaY = 0.0;
	float32 Value = 0.0f;
};

enum class ControllerButton : uint8
{
	DPadUp,
	DPadDown,
	DPadLeft,
	DPadRight,
	Start,
	Back,
	LeftThumb,
	RightThumb,
	LeftShoulder,
	RightShoulder,
	FaceDown,
	FaceRight,
	FaceLeft,
	FaceUp,
	Count
};

enum class ControllerAxis : uint8
{
	LeftX,
	LeftY,
	RightX,
	RightY,
	LeftTrigger,
	RightTrigger,
	Count
};

enum class InputDeviceFilter : uint8
{
	None = 0,
	Keyboard = 1 << 0,
	Mouse = 1 << 1,
	Controller = 1 << 2,
	Touch = 1 << 3,
	Pen = 1 << 4,
	Any = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4)
};

[[nodiscard]] constexpr InputDeviceFilter operator|(const InputDeviceFilter Left, const InputDeviceFilter Right) noexcept
{
	return static_cast<InputDeviceFilter>(static_cast<uint8>(Left) | static_cast<uint8>(Right));
}

[[nodiscard]] constexpr bool Contains(const InputDeviceFilter Value, const InputDeviceFilter Flag) noexcept
{
	return (static_cast<uint8>(Value) & static_cast<uint8>(Flag)) == static_cast<uint8>(Flag);
}

enum class Modifier : uint8
{
	None = 0,
	Shift = 1 << 0,
	Control = 1 << 1,
	Alt = 1 << 2,
	Super = 1 << 3,
	CapsLock = 1 << 4,
	NumLock = 1 << 5
};

[[nodiscard]] constexpr Modifier operator|(const Modifier Left, const Modifier Right) noexcept
{
	return static_cast<Modifier>(static_cast<uint8>(Left) | static_cast<uint8>(Right));
}

[[nodiscard]] constexpr bool Contains(const Modifier Value, const Modifier Flag) noexcept
{
	return (static_cast<uint8>(Value) & static_cast<uint8>(Flag)) == static_cast<uint8>(Flag);
}

struct ButtonSnapshot final
{
	bool Down = false;
	bool Pressed = false;
	bool Released = false;
	uint32 RepeatCount = 0;
};

struct ControllerCapabilities final
{
	ControllerID ID;
	bool Connected = false;
	bool SupportsHaptics = false;
	uint8 Subtype = 0;
	bool Wireless = false;
	uint8 BatteryLevel = 0;
	auto operator<=>(const ControllerCapabilities &) const = default;
};

struct HapticState final
{
	float32 LowFrequency = 0.0f;
	float32 HighFrequency = 0.0f;
};

struct ControllerSnapshot final
{
	ControllerID ID;
	bool Connected = false;
	bool ConnectedThisFrame = false;
	bool DisconnectedThisFrame = false;
	uint32 PacketNumber = 0;
	std::array<ButtonSnapshot, static_cast<usize>(ControllerButton::Count)> Buttons{};
	std::array<float32, static_cast<usize>(ControllerAxis::Count)> Axes{};
};

struct ControllerStateEvent final
{
	ControllerID ID;
	uint32 PacketNumber = 0;
	std::array<bool, static_cast<usize>(ControllerButton::Count)> Buttons{};
	std::array<float32, static_cast<usize>(ControllerAxis::Count)> Axes{};
};

struct InputEvent final
{
	InputEventType Type = InputEventType::Key;
	WindowID Window;
	LocalUserID User;
	Key Key = Key::Unknown;
	PhysicalKey PhysicalKey;
	MouseButton MouseButton = MouseButton::Left;
	InputState State = InputState::Released;
	Modifier Modifiers = Modifier::None;
	uint32 Codepoint = 0;
	float64 X = 0.0;
	float64 Y = 0.0;
	std::u32string Composition;
	uint32 CompositionCursor = 0;
	std::vector<std::u32string> CompositionCandidates;
	uint32 SelectedCompositionCandidate = 0;
	TouchContact Touch;
	PenContact Pen;
	Gesture Gesture;
	ControllerCapabilities ControllerCapabilities;
	ControllerStateEvent ControllerState;
};

class InputSnapshot final
{
  public:
	[[nodiscard]] const ButtonSnapshot &GetKey(Key Key) const;
	[[nodiscard]] const ButtonSnapshot &GetMouseButton(MouseButton Button) const;
	[[nodiscard]] const ButtonSnapshot &GetPhysicalKey(PhysicalKey Key) const;
	[[nodiscard]] bool IsKeyDown(Key Key) const;
	[[nodiscard]] bool WasKeyPressed(Key Key) const;
	[[nodiscard]] bool WasKeyReleased(Key Key) const;
	[[nodiscard]] bool IsMouseButtonDown(MouseButton Button) const;
	[[nodiscard]] float64 GetMouseX() const noexcept;
	[[nodiscard]] float64 GetMouseY() const noexcept;
	[[nodiscard]] float64 GetMouseDeltaX() const noexcept;
	[[nodiscard]] float64 GetMouseDeltaY() const noexcept;
	[[nodiscard]] float64 GetScrollX() const noexcept;
	[[nodiscard]] float64 GetScrollY() const noexcept;
	[[nodiscard]] const std::u32string &GetText() const noexcept;
	[[nodiscard]] WindowID GetWindowID() const noexcept;
	[[nodiscard]] LocalUserID GetUserID() const noexcept;
	[[nodiscard]] Modifier GetModifiers() const noexcept;
	[[nodiscard]] const ControllerSnapshot &GetController() const noexcept;
	[[nodiscard]] const std::u32string &GetComposition() const noexcept;
	[[nodiscard]] uint32 GetCompositionCursor() const noexcept;
	[[nodiscard]] std::span<const std::u32string> GetCompositionCandidates() const noexcept;
	[[nodiscard]] uint32 GetSelectedCompositionCandidate() const noexcept;
	[[nodiscard]] bool IsCursorInside() const noexcept;
	[[nodiscard]] std::span<const TouchContact> GetTouches() const noexcept;
	[[nodiscard]] const std::optional<PenContact> &GetPen() const noexcept;
	[[nodiscard]] std::span<const Gesture> GetGestures() const noexcept;

  private:
	friend class InputSystem;
	std::array<ButtonSnapshot, static_cast<usize>(Key::Count)> Keys{};
	std::array<ButtonSnapshot, static_cast<usize>(MouseButton::Count)> MouseButtons{};
	std::unordered_map<uint32, ButtonSnapshot> PhysicalKeys;
	std::u32string Text;
	WindowID Window;
	LocalUserID User;
	Modifier Modifiers = Modifier::None;
	ControllerSnapshot Controller;
	std::u32string Composition;
	uint32 CompositionCursor = 0;
	std::vector<std::u32string> CompositionCandidates;
	uint32 SelectedCompositionCandidate = 0;
	std::vector<TouchContact> Touches;
	std::optional<PenContact> Pen;
	std::vector<Gesture> Gestures;
	bool CursorInside = false;
	float64 MouseX = 0.0;
	float64 MouseY = 0.0;
	float64 MouseDeltaX = 0.0;
	float64 MouseDeltaY = 0.0;
	float64 ScrollX = 0.0;
	float64 ScrollY = 0.0;
};

using ActionID = uint64;
using InputContextID = uint64;

enum class BindingSource : uint8
{
	Key,
	PhysicalKey,
	MouseButton,
	MouseDeltaX,
	MouseDeltaY,
	ScrollX,
	ScrollY,
	ControllerButton,
	ControllerAxis,
	TouchContactCount,
	TouchPrimaryPressure,
	PenPressure,
	PenTiltX,
	PenTiltY,
	GesturePanX,
	GesturePanY,
	GestureZoom,
	GestureRotate
};

enum class ActionValueType : uint8
{
	Boolean,
	Axis1D,
	Axis2D,
	Axis3D
};
enum class ActionComponent : uint8
{
	X,
	Y,
	Z
};

struct InputAction final
{
	ActionID ID = 0;
	ActionValueType ValueType = ActionValueType::Boolean;
};

struct ChordInput final
{
	BindingSource Source = BindingSource::Key;
	Key Key = Key::Unknown;
	PhysicalKey PhysicalKey;
	MouseButton MouseButton = MouseButton::Left;
	ControllerButton ControllerButton = ControllerButton::FaceDown;
	bool RequiredDown = true;
	auto operator<=>(const ChordInput &) const = default;
};

struct InputBinding final
{
	ActionID Action = 0;
	BindingSource Source = BindingSource::Key;
	Key Key = Key::Unknown;
	PhysicalKey PhysicalKey;
	MouseButton MouseButton = MouseButton::Left;
	ControllerButton ControllerButton = ControllerButton::FaceDown;
	ControllerAxis ControllerAxis = ControllerAxis::LeftX;
	Modifier RequiredModifiers = Modifier::None;
	InputDeviceFilter Devices = InputDeviceFilter::Any;
	std::vector<ChordInput> Chord;
	ActionComponent Component = ActionComponent::X;
	float32 Scale = 1.0f;
	float32 DeadZone = 0.0f;
	float32 ResponseExponent = 1.0f;
	std::optional<LocalUserID> User;
};

struct InputComposite final
{
	ActionID Action = 0;
	std::vector<InputBinding> Parts;
};

struct ActionValue final
{
	float32 X = 0.0f;
	float32 Y = 0.0f;
	float32 Z = 0.0f;
	ActionValueType Type = ActionValueType::Boolean;

	[[nodiscard]] float32 Magnitude() const noexcept;
};

struct ActionState final
{
	ActionValue Value;
	bool Active = false;
	bool Triggered = false;
	bool Released = false;
};

class InputBindingConflictError final : public std::invalid_argument
{
  public:
	explicit InputBindingConflictError(const std::string &Diagnostic) : std::invalid_argument(Diagnostic)
	{
	}
};
} // namespace core::input

template <> struct std::hash<core::input::LocalUserID>
{
	usize operator()(const core::input::LocalUserID ID) const noexcept
	{
		return std::hash<uint8>{}(ID.Value);
	}
};
