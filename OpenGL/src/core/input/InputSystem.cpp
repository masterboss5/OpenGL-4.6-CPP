#include "InputSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "src/core/window/WindowManager.h"

#include <Windows.h>
#include <Xinput.h>

namespace core::input
{
namespace
{
const ButtonSnapshot EmptyButton{};
const InputSnapshot EmptySnapshot{};

[[nodiscard]] usize KeyIndex(const Key Key) noexcept
{
	return static_cast<usize>(Key);
}
[[nodiscard]] usize MouseButtonIndex(const MouseButton Button) noexcept
{
	return static_cast<usize>(Button);
}
[[nodiscard]] usize ControllerButtonIndex(const ControllerButton Button) noexcept
{
	return static_cast<usize>(Button);
}
[[nodiscard]] usize ControllerAxisIndex(const ControllerAxis Axis) noexcept
{
	return static_cast<usize>(Axis);
}
[[nodiscard]] uint32 PhysicalKeyIndex(const PhysicalKey Key) noexcept
{
	return (static_cast<uint32>(Key.ScanCode) << 1U) | static_cast<uint32>(Key.Extended);
}

constexpr std::array<uint16, static_cast<usize>(ControllerButton::Count)> ControllerButtonMasks{XINPUT_GAMEPAD_DPAD_UP,
																								XINPUT_GAMEPAD_DPAD_DOWN,
																								XINPUT_GAMEPAD_DPAD_LEFT,
																								XINPUT_GAMEPAD_DPAD_RIGHT,
																								XINPUT_GAMEPAD_START,
																								XINPUT_GAMEPAD_BACK,
																								XINPUT_GAMEPAD_LEFT_THUMB,
																								XINPUT_GAMEPAD_RIGHT_THUMB,
																								XINPUT_GAMEPAD_LEFT_SHOULDER,
																								XINPUT_GAMEPAD_RIGHT_SHOULDER,
																								XINPUT_GAMEPAD_A,
																								XINPUT_GAMEPAD_B,
																								XINPUT_GAMEPAD_X,
																								XINPUT_GAMEPAD_Y};

[[nodiscard]] float32 NormalizeThumb(const int16 Value) noexcept
{
	return Value < 0 ? static_cast<float32>(Value) / 32768.0f : static_cast<float32>(Value) / 32767.0f;
}

void UpdateButton(ButtonSnapshot &Button, const InputState State) noexcept
{
	if (State == InputState::Pressed)
	{
		Button.Pressed = !Button.Down;
		Button.Down = true;
	}
	else if (State == InputState::Released)
	{
		Button.Released = Button.Down;
		Button.Down = false;
	}
	else
	{
		Button.Down = true;
		++Button.RepeatCount;
	}
}

[[nodiscard]] bool ChordInputDown(const ChordInput &Input, const InputSnapshot &Snapshot)
{
	bool Down = false;
	if (Input.Source == BindingSource::Key)
		Down = Snapshot.IsKeyDown(Input.Key);
	else if (Input.Source == BindingSource::PhysicalKey)
		Down = Snapshot.GetPhysicalKey(Input.PhysicalKey).Down;
	else if (Input.Source == BindingSource::MouseButton)
		Down = Snapshot.IsMouseButtonDown(Input.MouseButton);
	else if (Input.Source == BindingSource::ControllerButton)
		Down = Snapshot.GetController().Buttons[ControllerButtonIndex(Input.ControllerButton)].Down;
	return Down == Input.RequiredDown;
}

[[nodiscard]] InputDeviceFilter RequiredDevice(const BindingSource Source) noexcept
{
	if (Source == BindingSource::Key || Source == BindingSource::PhysicalKey)
		return InputDeviceFilter::Keyboard;
	if (Source == BindingSource::MouseButton || Source == BindingSource::MouseDeltaX || Source == BindingSource::MouseDeltaY ||
		Source == BindingSource::ScrollX || Source == BindingSource::ScrollY)
		return InputDeviceFilter::Mouse;
	if (Source == BindingSource::ControllerButton || Source == BindingSource::ControllerAxis)
		return InputDeviceFilter::Controller;
	if (Source == BindingSource::TouchContactCount || Source == BindingSource::TouchPrimaryPressure ||
		Source == BindingSource::GesturePanX || Source == BindingSource::GesturePanY || Source == BindingSource::GestureZoom ||
		Source == BindingSource::GestureRotate)
		return InputDeviceFilter::Touch;
	return InputDeviceFilter::Pen;
}

[[nodiscard]] bool SamePhysicalSource(const InputBinding &Left, const InputBinding &Right) noexcept
{
	if (Left.Source != Right.Source)
		return false;
	if (Left.Source == BindingSource::Key)
		return Left.Key == Right.Key;
	if (Left.Source == BindingSource::PhysicalKey)
		return Left.PhysicalKey == Right.PhysicalKey;
	if (Left.Source == BindingSource::MouseButton)
		return Left.MouseButton == Right.MouseButton;
	if (Left.Source == BindingSource::ControllerButton)
		return Left.ControllerButton == Right.ControllerButton;
	if (Left.Source == BindingSource::ControllerAxis)
		return Left.ControllerAxis == Right.ControllerAxis;
	return true;
}
} // namespace

const ButtonSnapshot &InputSnapshot::GetKey(const Key Key) const
{
	const usize Index = KeyIndex(Key);
	return Index < this->Keys.size() ? this->Keys[Index] : EmptyButton;
}
const ButtonSnapshot &InputSnapshot::GetMouseButton(const MouseButton Button) const
{
	const usize Index = MouseButtonIndex(Button);
	return Index < this->MouseButtons.size() ? this->MouseButtons[Index] : EmptyButton;
}
const ButtonSnapshot &InputSnapshot::GetPhysicalKey(const PhysicalKey Key) const
{
	const auto Iterator = this->PhysicalKeys.find(PhysicalKeyIndex(Key));
	return Iterator == this->PhysicalKeys.end() ? EmptyButton : Iterator->second;
}
bool InputSnapshot::IsKeyDown(const Key Key) const
{
	return this->GetKey(Key).Down;
}
bool InputSnapshot::WasKeyPressed(const Key Key) const
{
	return this->GetKey(Key).Pressed;
}
bool InputSnapshot::WasKeyReleased(const Key Key) const
{
	return this->GetKey(Key).Released;
}
bool InputSnapshot::IsMouseButtonDown(const MouseButton Button) const
{
	return this->GetMouseButton(Button).Down;
}
float64 InputSnapshot::GetMouseX() const noexcept
{
	return this->MouseX;
}
float64 InputSnapshot::GetMouseY() const noexcept
{
	return this->MouseY;
}
float64 InputSnapshot::GetMouseDeltaX() const noexcept
{
	return this->MouseDeltaX;
}
float64 InputSnapshot::GetMouseDeltaY() const noexcept
{
	return this->MouseDeltaY;
}
float64 InputSnapshot::GetScrollX() const noexcept
{
	return this->ScrollX;
}
float64 InputSnapshot::GetScrollY() const noexcept
{
	return this->ScrollY;
}
const std::u32string &InputSnapshot::GetText() const noexcept
{
	return this->Text;
}
WindowID InputSnapshot::GetWindowID() const noexcept
{
	return this->Window;
}
LocalUserID InputSnapshot::GetUserID() const noexcept
{
	return this->User;
}
Modifier InputSnapshot::GetModifiers() const noexcept
{
	return this->Modifiers;
}
const ControllerSnapshot &InputSnapshot::GetController() const noexcept
{
	return this->Controller;
}
const std::u32string &InputSnapshot::GetComposition() const noexcept
{
	return this->Composition;
}
uint32 InputSnapshot::GetCompositionCursor() const noexcept
{
	return this->CompositionCursor;
}
std::span<const std::u32string> InputSnapshot::GetCompositionCandidates() const noexcept
{
	return this->CompositionCandidates;
}
uint32 InputSnapshot::GetSelectedCompositionCandidate() const noexcept
{
	return this->SelectedCompositionCandidate;
}
bool InputSnapshot::IsCursorInside() const noexcept
{
	return this->CursorInside;
}
std::span<const TouchContact> InputSnapshot::GetTouches() const noexcept
{
	return this->Touches;
}
const std::optional<PenContact> &InputSnapshot::GetPen() const noexcept
{
	return this->Pen;
}
std::span<const Gesture> InputSnapshot::GetGestures() const noexcept
{
	return this->Gestures;
}

float32 ActionValue::Magnitude() const noexcept
{
	if (this->Type == ActionValueType::Boolean || this->Type == ActionValueType::Axis1D)
		return std::abs(this->X);
	if (this->Type == ActionValueType::Axis2D)
		return std::sqrt(this->X * this->X + this->Y * this->Y);
	return std::sqrt(this->X * this->X + this->Y * this->Y + this->Z * this->Z);
}

InputSystem::~InputSystem()
{
	this->StopAllHaptics();
}

void InputSystem::BeginFrame(WindowManager &WindowManager)
{
	for (auto &[key, snapshot] : this->Snapshots)
	{
		for (ButtonSnapshot &State : snapshot.Keys)
		{
			State.Pressed = false;
			State.Released = false;
			State.RepeatCount = 0;
		}
		for (ButtonSnapshot &State : snapshot.MouseButtons)
		{
			State.Pressed = false;
			State.Released = false;
			State.RepeatCount = 0;
		}
		for (auto &[physical, state] : snapshot.PhysicalKeys)
		{
			state.Pressed = false;
			state.Released = false;
			state.RepeatCount = 0;
		}
		snapshot.MouseDeltaX = 0.0;
		snapshot.MouseDeltaY = 0.0;
		snapshot.ScrollX = 0.0;
		snapshot.ScrollY = 0.0;
		snapshot.Text.clear();
		snapshot.Controller.ConnectedThisFrame = false;
		snapshot.Controller.DisconnectedThisFrame = false;
		for (ButtonSnapshot &State : snapshot.Controller.Buttons)
		{
			State.Pressed = false;
			State.Released = false;
			State.RepeatCount = 0;
		}
		snapshot.Touches.erase(std::remove_if(snapshot.Touches.begin(), snapshot.Touches.end(), [](const TouchContact &Contact)
											  { return Contact.Phase == ContactPhase::Ended || Contact.Phase == ContactPhase::Cancelled; }),
							   snapshot.Touches.end());
		for (TouchContact &Contact : snapshot.Touches)
			if (Contact.Phase == ContactPhase::Began)
				Contact.Phase = ContactPhase::Moved;
		if (snapshot.Pen && (snapshot.Pen->Phase == ContactPhase::Ended || snapshot.Pen->Phase == ContactPhase::Cancelled))
			snapshot.Pen.reset();
		else if (snapshot.Pen && snapshot.Pen->Phase == ContactPhase::Began)
			snapshot.Pen->Phase = ContactPhase::Moved;
		snapshot.Gestures.clear();
	}

	this->PollControllers(WindowManager);
	for (InputEvent &Event : WindowManager.ConsumeInputEvents())
	{
		SnapshotKey Key{Event.Window, Event.User};
		InputSnapshot &Snapshot = this->Snapshots[Key];
		Snapshot.Window = Event.Window;
		Snapshot.User = Event.User;
		if (Event.Type == InputEventType::Key)
		{
			Snapshot.Modifiers = Event.Modifiers;
			if (KeyIndex(Event.Key) < Snapshot.Keys.size())
				UpdateButton(Snapshot.Keys[KeyIndex(Event.Key)], Event.State);
			if (Event.PhysicalKey.ScanCode != 0)
				UpdateButton(Snapshot.PhysicalKeys[PhysicalKeyIndex(Event.PhysicalKey)], Event.State);
		}
		else if (Event.Type == InputEventType::Text)
		{
			if (Event.Codepoint <= 0x10FFFFU && (Event.Codepoint < 0xD800U || Event.Codepoint > 0xDFFFU))
				Snapshot.Text.push_back(static_cast<char32_t>(Event.Codepoint));
		}
		else if (Event.Type == InputEventType::MouseButton)
		{
			Snapshot.Modifiers = Event.Modifiers;
			if (MouseButtonIndex(Event.MouseButton) < Snapshot.MouseButtons.size())
				UpdateButton(Snapshot.MouseButtons[MouseButtonIndex(Event.MouseButton)], Event.State);
		}
		else if (Event.Type == InputEventType::MouseMove)
		{
			Snapshot.MouseDeltaX += Event.X - Snapshot.MouseX;
			Snapshot.MouseDeltaY += Event.Y - Snapshot.MouseY;
			Snapshot.MouseX = Event.X;
			Snapshot.MouseY = Event.Y;
		}
		else if (Event.Type == InputEventType::MouseScroll)
		{
			Snapshot.ScrollX += Event.X;
			Snapshot.ScrollY += Event.Y;
		}
		else if (Event.Type == InputEventType::CursorEntered)
			Snapshot.CursorInside = Event.State != InputState::Released;
		else if (Event.Type == InputEventType::IMEComposition)
		{
			Snapshot.Composition = std::move(Event.Composition);
			Snapshot.CompositionCursor = Event.CompositionCursor;
			Snapshot.CompositionCandidates = std::move(Event.CompositionCandidates);
			Snapshot.SelectedCompositionCandidate = Event.SelectedCompositionCandidate;
		}
		else if (Event.Type == InputEventType::Touch)
		{
			const auto Contact = std::find_if(Snapshot.Touches.begin(), Snapshot.Touches.end(),
											  [&Event](const TouchContact &Candidate) { return Candidate.ID == Event.Touch.ID; });
			if (Contact == Snapshot.Touches.end())
				Snapshot.Touches.push_back(Event.Touch);
			else
				*Contact = Event.Touch;
		}
		else if (Event.Type == InputEventType::Pen)
			Snapshot.Pen = Event.Pen;
		else if (Event.Type == InputEventType::Gesture)
			Snapshot.Gestures.push_back(Event.Gesture);
		else if (Event.Type == InputEventType::ControllerConnected || Event.Type == InputEventType::ControllerCapabilitiesChanged)
		{
			Snapshot.Controller.ID = Event.ControllerCapabilities.ID;
			Snapshot.Controller.ConnectedThisFrame = Event.Type == InputEventType::ControllerConnected && !Snapshot.Controller.Connected;
			Snapshot.Controller.Connected = true;
			this->ControllerCapabilitySlots[Event.ControllerCapabilities.ID.Value] = Event.ControllerCapabilities;
		}
		else if (Event.Type == InputEventType::ControllerDisconnected)
		{
			Snapshot.Controller.DisconnectedThisFrame = Snapshot.Controller.Connected;
			Snapshot.Controller.Connected = false;
			Snapshot.Controller.PacketNumber = 0;
			for (ButtonSnapshot &Button : Snapshot.Controller.Buttons)
			{
				Button.Released = Button.Down;
				Button.Down = false;
			}
			Snapshot.Controller.Axes.fill(0.0f);
			this->ControllerCapabilitySlots[Event.ControllerState.ID.Value] = ControllerCapabilities{.ID = Event.ControllerState.ID};
		}
		else if (Event.Type == InputEventType::ControllerState)
		{
			ControllerSnapshot &Controller = Snapshot.Controller;
			Controller.ID = Event.ControllerState.ID;
			Controller.Connected = true;
			Controller.PacketNumber = Event.ControllerState.PacketNumber;
			for (usize Index = 0; Index < Controller.Buttons.size(); ++Index)
			{
				ButtonSnapshot &Button = Controller.Buttons[Index];
				const bool Down = Event.ControllerState.Buttons[Index];
				Button.Pressed = Down && !Button.Down;
				Button.Released = !Down && Button.Down;
				Button.Down = Down;
			}
			Controller.Axes = Event.ControllerState.Axes;
		}
		else if (Event.Type == InputEventType::FocusLost)
		{
			Snapshot.Modifiers = Modifier::None;
			Snapshot.CursorInside = false;
			for (ButtonSnapshot &State : Snapshot.Keys)
			{
				State.Released = State.Down;
				State.Down = false;
			}
			for (ButtonSnapshot &State : Snapshot.MouseButtons)
			{
				State.Released = State.Down;
				State.Down = false;
			}
			for (auto &[physical, state] : Snapshot.PhysicalKeys)
			{
				state.Released = state.Down;
				state.Down = false;
			}
		}
	}
}

const InputSnapshot &InputSystem::GetSnapshot(const WindowID Window, const LocalUserID User) const
{
	const auto Iterator = this->Snapshots.find({Window, User});
	return Iterator == this->Snapshots.end() ? EmptySnapshot : Iterator->second;
}

void InputSystem::DefineAction(const InputAction Action)
{
	if (Action.ID == 0)
		throw std::invalid_argument("Input action ID must be non-zero");
	if (!this->Actions.emplace(Action.ID, Action).second)
		throw std::invalid_argument("Input action ID is already defined");
}

void InputSystem::RemoveAction(const ActionID Action)
{
	if (this->Actions.erase(Action) == 0)
		throw std::out_of_range("Unknown input action");
	for (ActionContext &Context : this->Contexts)
		std::erase_if(Context.Bindings, [Action](const InputBinding &Binding) { return Binding.Action == Action; });
}

void InputSystem::CreateContext(const InputContextID ID, const int32 Priority)
{
	if (ID == 0)
		throw std::invalid_argument("Input context ID must be non-zero");
	if (std::find_if(this->Contexts.begin(), this->Contexts.end(), [ID](const ActionContext &Context) { return Context.ID == ID; }) !=
		this->Contexts.end())
		throw std::invalid_argument("Input context ID is already registered");
	this->Contexts.push_back({.ID = ID, .Priority = Priority});
	std::ranges::stable_sort(this->Contexts, std::greater{}, &ActionContext::Priority);
}

void InputSystem::RemoveContext(const InputContextID ID)
{
	const auto Iterator =
		std::remove_if(this->Contexts.begin(), this->Contexts.end(), [ID](const ActionContext &Context) { return Context.ID == ID; });
	if (Iterator == this->Contexts.end())
		throw std::out_of_range("Unknown input context");
	this->Contexts.erase(Iterator, this->Contexts.end());
}

void InputSystem::SetContextEnabled(const InputContextID ID, const bool Enabled)
{
	this->RequireContext(ID).Enabled = Enabled;
}

void InputSystem::ValidateBinding(const InputBinding &Binding) const
{
	const auto Action = this->Actions.find(Binding.Action);
	if (Binding.Action == 0 || Action == this->Actions.end())
		throw std::invalid_argument("Input binding references an undefined action");
	if (!std::isfinite(Binding.Scale) || !std::isfinite(Binding.ResponseExponent) || Binding.ResponseExponent <= 0.0f)
		throw std::invalid_argument("Input binding scale and response exponent must be finite, with a positive exponent");
	if (!std::isfinite(Binding.DeadZone) || Binding.DeadZone < 0.0f || Binding.DeadZone >= 1.0f)
		throw std::invalid_argument("Input binding dead zone must be in the [0, 1) range");
	if (!Contains(Binding.Devices, RequiredDevice(Binding.Source)))
		throw std::invalid_argument("Input binding device filter excludes its selected source");
	if ((Action->second.ValueType == ActionValueType::Boolean || Action->second.ValueType == ActionValueType::Axis1D) &&
		Binding.Component != ActionComponent::X)
		throw std::invalid_argument("Boolean and one-dimensional actions only accept the X component");
	if (Action->second.ValueType == ActionValueType::Axis2D && Binding.Component == ActionComponent::Z)
		throw std::invalid_argument("Two-dimensional actions do not accept the Z component");
	if (Binding.Source == BindingSource::Key && Binding.Key == Key::Unknown)
		throw std::invalid_argument("Logical-key bindings require a known key");
	if (Binding.Source == BindingSource::PhysicalKey && Binding.PhysicalKey.ScanCode == 0)
		throw std::invalid_argument("Physical-key bindings require a non-zero scan code");
	for (const ChordInput &Chord : Binding.Chord)
	{
		if (Chord.Source != BindingSource::Key && Chord.Source != BindingSource::PhysicalKey &&
			Chord.Source != BindingSource::MouseButton && Chord.Source != BindingSource::ControllerButton)
			throw std::invalid_argument("Chord inputs must be digital key, mouse-button, or controller-button sources");
		if (Chord.Source == BindingSource::Key && Chord.Key == Key::Unknown)
			throw std::invalid_argument("Chord logical keys must be known");
		if (Chord.Source == BindingSource::PhysicalKey && Chord.PhysicalKey.ScanCode == 0)
			throw std::invalid_argument("Chord physical keys require a non-zero scan code");
	}
}

void InputSystem::AddBinding(const InputContextID ContextID, InputBinding Binding)
{
	this->ValidateBinding(Binding);
	ActionContext &Context = this->RequireContext(ContextID);
	if (std::find_if(Context.Bindings.begin(), Context.Bindings.end(),
					 [&Binding](const InputBinding &Existing) { return Conflicts(Existing, Binding); }) != Context.Bindings.end())
		throw InputBindingConflictError("Input binding conflicts with an existing binding in context " + std::to_string(ContextID));
	Context.Bindings.push_back(std::move(Binding));
}

void InputSystem::AddComposite(const InputContextID ContextID, InputComposite Composite)
{
	if (Composite.Action == 0 || !this->Actions.contains(Composite.Action))
		throw std::invalid_argument("Input composite references an undefined action");
	if (Composite.Parts.empty())
		throw std::invalid_argument("Input composites require at least one part");
	ActionContext &Context = this->RequireContext(ContextID);
	for (InputBinding &Part : Composite.Parts)
	{
		Part.Action = Composite.Action;
		this->ValidateBinding(Part);
	}
	for (usize First = 0; First < Composite.Parts.size(); ++First)
	{
		if (std::find_if(Context.Bindings.begin(), Context.Bindings.end(), [&Part = Composite.Parts[First]](const InputBinding &Existing)
						 { return Conflicts(Existing, Part); }) != Context.Bindings.end())
			throw InputBindingConflictError("Input composite conflicts with an existing binding in context " + std::to_string(ContextID));
		for (usize Second = First + 1; Second < Composite.Parts.size(); ++Second)
			if (Conflicts(Composite.Parts[First], Composite.Parts[Second]))
				throw InputBindingConflictError("Input composite contains conflicting parts");
	}
	Context.Bindings.insert(Context.Bindings.end(), std::make_move_iterator(Composite.Parts.begin()),
							std::make_move_iterator(Composite.Parts.end()));
}

void InputSystem::Rebind(const InputContextID ContextID, const ActionID Action, InputBinding Replacement)
{
	ActionContext &Context = this->RequireContext(ContextID);
	const auto Iterator = std::find_if(Context.Bindings.begin(), Context.Bindings.end(),
									   [Action](const InputBinding &Binding) { return Binding.Action == Action; });
	if (Iterator == Context.Bindings.end())
		throw std::out_of_range("Cannot rebind unknown action");
	Replacement.Action = Action;
	this->ValidateBinding(Replacement);
	const auto Conflict =
		std::find_if(Context.Bindings.begin(), Context.Bindings.end(), [&Replacement, &Iterator](const InputBinding &Existing)
					 { return &Existing != &*Iterator && Conflicts(Existing, Replacement); });
	if (Conflict != Context.Bindings.end())
		throw InputBindingConflictError("Replacement input binding conflicts with an existing binding");
	*Iterator = std::move(Replacement);
}

void InputSystem::RemoveBinding(const InputContextID ContextID, const ActionID Action)
{
	ActionContext &Context = this->RequireContext(ContextID);
	const auto Iterator = std::remove_if(Context.Bindings.begin(), Context.Bindings.end(),
										 [Action](const InputBinding &Binding) { return Binding.Action == Action; });
	if (Iterator == Context.Bindings.end())
		throw std::out_of_range("Cannot remove unknown action binding");
	Context.Bindings.erase(Iterator, Context.Bindings.end());
}

ActionState InputSystem::GetAction(const ActionID ActionID, const WindowID Window, const LocalUserID User) const
{
	const auto Definition = this->Actions.find(ActionID);
	if (Definition == this->Actions.end())
		throw std::out_of_range("Unknown input action");
	const InputSnapshot &Snapshot = this->GetSnapshot(Window, User);
	ActionState Result;
	Result.Value.Type = Definition->second.ValueType;
	for (const ActionContext &Context : this->Contexts)
	{
		if (!Context.Enabled)
			continue;
		bool MatchedInContext = false;
		for (const InputBinding &Binding : Context.Bindings)
		{
			if (Binding.Action != ActionID || (Binding.User && *Binding.User != User))
				continue;
			MatchedInContext = true;
			const float32 Value = Evaluate(Binding, Snapshot);
			if (Binding.Component == ActionComponent::X)
				Result.Value.X += Value;
			else if (Binding.Component == ActionComponent::Y)
				Result.Value.Y += Value;
			else
				Result.Value.Z += Value;
			Result.Triggered = Result.Triggered || IsTriggered(Binding, Snapshot);
			Result.Released = Result.Released || IsReleased(Binding, Snapshot);
		}
		if (MatchedInContext)
			break;
	}
	Result.Value.X = std::clamp(Result.Value.X, -1.0f, 1.0f);
	Result.Value.Y = std::clamp(Result.Value.Y, -1.0f, 1.0f);
	Result.Value.Z = std::clamp(Result.Value.Z, -1.0f, 1.0f);
	if (Definition->second.ValueType == ActionValueType::Boolean)
		Result.Value.X = Result.Value.X == 0.0f ? 0.0f : 1.0f;
	Result.Active = Result.Value.Magnitude() > 0.0f;
	return Result;
}

usize InputSystem::SnapshotKeyHash::operator()(const SnapshotKey &Key) const noexcept
{
	return std::hash<WindowID>{}(Key.Window) ^ (std::hash<LocalUserID>{}(Key.User) << 1);
}
bool InputSystem::Conflicts(const InputBinding &Left, const InputBinding &Right) noexcept
{
	return SamePhysicalSource(Left, Right) && Left.RequiredModifiers == Right.RequiredModifiers && Left.Chord == Right.Chord &&
		   Left.User == Right.User && static_cast<uint8>(Left.Devices) == static_cast<uint8>(Right.Devices);
}

float32 InputSystem::Evaluate(const InputBinding &Binding, const InputSnapshot &Snapshot)
{
	if (!Contains(Snapshot.GetModifiers(), Binding.RequiredModifiers))
		return 0.0f;
	for (const ChordInput &Chord : Binding.Chord)
		if (!ChordInputDown(Chord, Snapshot))
			return 0.0f;
	float32 Value = 0.0f;
	if (Binding.Source == BindingSource::Key)
		Value = Snapshot.IsKeyDown(Binding.Key) ? 1.0f : 0.0f;
	else if (Binding.Source == BindingSource::PhysicalKey)
		Value = Snapshot.GetPhysicalKey(Binding.PhysicalKey).Down ? 1.0f : 0.0f;
	else if (Binding.Source == BindingSource::MouseButton)
		Value = Snapshot.IsMouseButtonDown(Binding.MouseButton) ? 1.0f : 0.0f;
	else if (Binding.Source == BindingSource::MouseDeltaX)
		Value = static_cast<float32>(Snapshot.GetMouseDeltaX());
	else if (Binding.Source == BindingSource::MouseDeltaY)
		Value = static_cast<float32>(Snapshot.GetMouseDeltaY());
	else if (Binding.Source == BindingSource::ScrollX)
		Value = static_cast<float32>(Snapshot.GetScrollX());
	else if (Binding.Source == BindingSource::ScrollY)
		Value = static_cast<float32>(Snapshot.GetScrollY());
	else if (Binding.Source == BindingSource::ControllerButton)
		Value = Snapshot.GetController().Buttons[ControllerButtonIndex(Binding.ControllerButton)].Down ? 1.0f : 0.0f;
	else if (Binding.Source == BindingSource::ControllerAxis)
		Value = Snapshot.GetController().Axes[ControllerAxisIndex(Binding.ControllerAxis)];
	else if (Binding.Source == BindingSource::TouchContactCount)
		Value = static_cast<float32>(Snapshot.GetTouches().size());
	else if (Binding.Source == BindingSource::TouchPrimaryPressure)
	{
		const auto Primary = std::find_if(Snapshot.GetTouches().begin(), Snapshot.GetTouches().end(),
										  [](const TouchContact &Contact) { return Contact.Primary; });
		if (Primary != Snapshot.GetTouches().end())
			Value = Primary->Pressure;
	}
	else if (Binding.Source == BindingSource::PenPressure && Snapshot.GetPen())
		Value = Snapshot.GetPen()->Pressure;
	else if (Binding.Source == BindingSource::PenTiltX && Snapshot.GetPen())
		Value = Snapshot.GetPen()->TiltX / 90.0f;
	else if (Binding.Source == BindingSource::PenTiltY && Snapshot.GetPen())
		Value = Snapshot.GetPen()->TiltY / 90.0f;
	else
	{
		for (const Gesture &Gesture : Snapshot.GetGestures())
		{
			if (Binding.Source == BindingSource::GesturePanX && Gesture.Type == GestureType::Pan)
				Value += static_cast<float32>(Gesture.DeltaX);
			else if (Binding.Source == BindingSource::GesturePanY && Gesture.Type == GestureType::Pan)
				Value += static_cast<float32>(Gesture.DeltaY);
			else if (Binding.Source == BindingSource::GestureZoom && Gesture.Type == GestureType::Zoom)
				Value += Gesture.Value - 1.0f;
			else if (Binding.Source == BindingSource::GestureRotate && Gesture.Type == GestureType::Rotate)
				Value += Gesture.Value;
		}
	}
	const float32 Magnitude = std::abs(Value);
	if (Magnitude <= Binding.DeadZone)
		return 0.0f;
	const float32 Normalized = Binding.DeadZone == 0.0f ? Magnitude : (Magnitude - Binding.DeadZone) / (1.0f - Binding.DeadZone);
	return std::copysign(std::pow(Normalized, Binding.ResponseExponent) * Binding.Scale, Value);
}

bool InputSystem::IsTriggered(const InputBinding &Binding, const InputSnapshot &Snapshot)
{
	if (Binding.Source == BindingSource::Key)
		return Snapshot.GetKey(Binding.Key).Pressed;
	if (Binding.Source == BindingSource::PhysicalKey)
		return Snapshot.GetPhysicalKey(Binding.PhysicalKey).Pressed;
	if (Binding.Source == BindingSource::MouseButton)
		return Snapshot.GetMouseButton(Binding.MouseButton).Pressed;
	if (Binding.Source == BindingSource::ControllerButton)
		return Snapshot.GetController().Buttons[ControllerButtonIndex(Binding.ControllerButton)].Pressed;
	if (Binding.Source == BindingSource::TouchContactCount || Binding.Source == BindingSource::TouchPrimaryPressure)
		return std::ranges::any_of(Snapshot.GetTouches(), [](const TouchContact &Contact) { return Contact.Phase == ContactPhase::Began; });
	if (Binding.Source == BindingSource::PenPressure || Binding.Source == BindingSource::PenTiltX ||
		Binding.Source == BindingSource::PenTiltY)
		return Snapshot.GetPen() && Snapshot.GetPen()->Phase == ContactPhase::Began;
	if (Binding.Source == BindingSource::GesturePanX || Binding.Source == BindingSource::GesturePanY ||
		Binding.Source == BindingSource::GestureZoom || Binding.Source == BindingSource::GestureRotate)
		return Evaluate(Binding, Snapshot) != 0.0f;
	return false;
}

bool InputSystem::IsReleased(const InputBinding &Binding, const InputSnapshot &Snapshot)
{
	if (Binding.Source == BindingSource::Key)
		return Snapshot.GetKey(Binding.Key).Released;
	if (Binding.Source == BindingSource::PhysicalKey)
		return Snapshot.GetPhysicalKey(Binding.PhysicalKey).Released;
	if (Binding.Source == BindingSource::MouseButton)
		return Snapshot.GetMouseButton(Binding.MouseButton).Released;
	if (Binding.Source == BindingSource::ControllerButton)
		return Snapshot.GetController().Buttons[ControllerButtonIndex(Binding.ControllerButton)].Released;
	if (Binding.Source == BindingSource::TouchContactCount || Binding.Source == BindingSource::TouchPrimaryPressure)
		return std::ranges::any_of(Snapshot.GetTouches(), [](const TouchContact &Contact)
								   { return Contact.Phase == ContactPhase::Ended || Contact.Phase == ContactPhase::Cancelled; });
	if (Binding.Source == BindingSource::PenPressure || Binding.Source == BindingSource::PenTiltX ||
		Binding.Source == BindingSource::PenTiltY)
		return Snapshot.GetPen() &&
			   (Snapshot.GetPen()->Phase == ContactPhase::Ended || Snapshot.GetPen()->Phase == ContactPhase::Cancelled);
	return false;
}

ControllerCapabilities InputSystem::GetControllerCapabilities(const ControllerID Controller) const
{
	if (Controller.Value >= this->ControllerCapabilitySlots.size())
		throw std::out_of_range("ControllerID exceeds the supported local controller range");
	return this->ControllerCapabilitySlots[Controller.Value].value_or(ControllerCapabilities{.ID = Controller});
}

void InputSystem::SetControllerHaptics(const ControllerID Controller, const HapticState State)
{
	if (Controller.Value >= this->ControllerCapabilitySlots.size())
		throw std::out_of_range("ControllerID exceeds the supported local controller range");
	if (!std::isfinite(State.LowFrequency) || !std::isfinite(State.HighFrequency) || State.LowFrequency < 0.0f ||
		State.LowFrequency > 1.0f || State.HighFrequency < 0.0f || State.HighFrequency > 1.0f)
		throw std::invalid_argument("Controller haptic strengths must be finite and in the [0, 1] range");
	const ControllerCapabilities Capabilities = this->GetControllerCapabilities(Controller);
	if (!Capabilities.Connected || !Capabilities.SupportsHaptics)
		throw std::runtime_error("Controller is unavailable or does not report haptic support");
	XINPUT_VIBRATION Vibration{.wLeftMotorSpeed = static_cast<uint16>(State.LowFrequency * 65535.0f),
							   .wRightMotorSpeed = static_cast<uint16>(State.HighFrequency * 65535.0f)};
	if (XInputSetState(Controller.Value, &Vibration) != ERROR_SUCCESS)
		throw std::runtime_error("Controller haptic submission failed");
}

void InputSystem::StopAllHaptics() noexcept
{
	XINPUT_VIBRATION Vibration{};
	for (uint32 Controller = 0; Controller < this->ControllerCapabilitySlots.size(); ++Controller)
		(void)XInputSetState(Controller, &Vibration);
}

void InputSystem::PollControllers(WindowManager &WindowManager)
{
	const WindowID Window = WindowManager.GetPrimaryWindow().GetID();
	const auto Now = std::chrono::steady_clock::now();
	for (uint32 ControllerIndex = 0; ControllerIndex < this->ControllerCapabilitySlots.size(); ++ControllerIndex)
	{
		const ControllerID ID{static_cast<uint8>(ControllerIndex)};
		XINPUT_STATE NativeState{};
		const bool Connected = XInputGetState(ControllerIndex, &NativeState) == ERROR_SUCCESS;
		const bool WasConnected =
			this->ControllerCapabilitySlots[ControllerIndex].has_value() && this->ControllerCapabilitySlots[ControllerIndex]->Connected;
		const LocalUserID User{static_cast<uint8>(ControllerIndex)};
		if (!Connected)
		{
			if (WasConnected)
				WindowManager.EnqueueInputEvent(
					{.Type = InputEventType::ControllerDisconnected, .Window = Window, .User = User, .ControllerState = {.ID = ID}});
			this->ControllerCapabilitySlots[ControllerIndex] = ControllerCapabilities{.ID = ID};
			this->ControllerPackets[ControllerIndex] = 0;
			continue;
		}

		const ControllerCapabilities PreviousCapabilities =
			this->ControllerCapabilitySlots[ControllerIndex].value_or(ControllerCapabilities{.ID = ID});
		ControllerCapabilities Capabilities = PreviousCapabilities;
		const bool RefreshCapabilities =
			!WasConnected || Now - this->ControllerCapabilityRefresh[ControllerIndex] >= std::chrono::seconds(5);
		if (RefreshCapabilities)
		{
			XINPUT_CAPABILITIES NativeCapabilities{};
			const bool CapabilitiesAvailable =
				XInputGetCapabilities(ControllerIndex, XINPUT_FLAG_GAMEPAD, &NativeCapabilities) == ERROR_SUCCESS;
			XINPUT_BATTERY_INFORMATION Battery{};
			const bool BatteryAvailable = XInputGetBatteryInformation(ControllerIndex, BATTERY_DEVTYPE_GAMEPAD, &Battery) == ERROR_SUCCESS;
			Capabilities = {.ID = ID,
							.Connected = true,
							.SupportsHaptics = CapabilitiesAvailable && (NativeCapabilities.Vibration.wLeftMotorSpeed != 0 ||
																		 NativeCapabilities.Vibration.wRightMotorSpeed != 0),
							.Subtype = CapabilitiesAvailable ? static_cast<uint8>(NativeCapabilities.SubType) : uint8{},
							.Wireless = BatteryAvailable && Battery.BatteryType != BATTERY_TYPE_WIRED &&
										Battery.BatteryType != BATTERY_TYPE_DISCONNECTED && Battery.BatteryType != BATTERY_TYPE_UNKNOWN,
							.BatteryLevel = BatteryAvailable ? static_cast<uint8>(Battery.BatteryLevel) : uint8{}};
			this->ControllerCapabilityRefresh[ControllerIndex] = Now;
		}
		else
			Capabilities.Connected = true;
		this->ControllerCapabilitySlots[ControllerIndex] = Capabilities;
		if (!WasConnected)
			WindowManager.EnqueueInputEvent(
				{.Type = InputEventType::ControllerConnected, .Window = Window, .User = User, .ControllerCapabilities = Capabilities});
		else if (Capabilities != PreviousCapabilities)
			WindowManager.EnqueueInputEvent({.Type = InputEventType::ControllerCapabilitiesChanged,
											 .Window = Window,
											 .User = User,
											 .ControllerCapabilities = Capabilities});
		if (WasConnected && this->ControllerPackets[ControllerIndex] == NativeState.dwPacketNumber)
			continue;

		ControllerStateEvent State{.ID = ID, .PacketNumber = NativeState.dwPacketNumber};
		for (usize Button = 0; Button < State.Buttons.size(); ++Button)
			State.Buttons[Button] = (NativeState.Gamepad.wButtons & ControllerButtonMasks[Button]) != 0;
		State.Axes[ControllerAxisIndex(ControllerAxis::LeftX)] = NormalizeThumb(NativeState.Gamepad.sThumbLX);
		State.Axes[ControllerAxisIndex(ControllerAxis::LeftY)] = NormalizeThumb(NativeState.Gamepad.sThumbLY);
		State.Axes[ControllerAxisIndex(ControllerAxis::RightX)] = NormalizeThumb(NativeState.Gamepad.sThumbRX);
		State.Axes[ControllerAxisIndex(ControllerAxis::RightY)] = NormalizeThumb(NativeState.Gamepad.sThumbRY);
		State.Axes[ControllerAxisIndex(ControllerAxis::LeftTrigger)] = static_cast<float32>(NativeState.Gamepad.bLeftTrigger) / 255.0f;
		State.Axes[ControllerAxisIndex(ControllerAxis::RightTrigger)] = static_cast<float32>(NativeState.Gamepad.bRightTrigger) / 255.0f;
		WindowManager.EnqueueInputEvent(
			{.Type = InputEventType::ControllerState, .Window = Window, .User = User, .ControllerState = State});
		this->ControllerPackets[ControllerIndex] = NativeState.dwPacketNumber;
	}
}

InputSystem::ActionContext &InputSystem::RequireContext(const InputContextID ID)
{
	const auto Iterator =
		std::find_if(this->Contexts.begin(), this->Contexts.end(), [ID](const ActionContext &Context) { return Context.ID == ID; });
	if (Iterator == this->Contexts.end())
		throw std::out_of_range("Unknown input context");
	return *Iterator;
}

const InputSystem::ActionContext &InputSystem::RequireContext(const InputContextID ID) const
{
	const auto Iterator =
		std::find_if(this->Contexts.begin(), this->Contexts.end(), [ID](const ActionContext &Context) { return Context.ID == ID; });
	if (Iterator == this->Contexts.end())
		throw std::out_of_range("Unknown input context");
	return *Iterator;
}
} // namespace core::input
