#include "EditorCameraController.h"

#include "Source/component/object/CObjectTransformComponent.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace editor::viewport
{
namespace
{
[[nodiscard]] bool IsFinitePositive(const float32 Value) noexcept
{
	return std::isfinite(Value) && Value > 0.0f;
}
} // namespace

void EditorCameraController::SetSettings(const EditorCameraSettings &Settings)
{
	this->ValidateSettings(Settings);
	this->Settings = Settings;
	this->OrbitDistance = std::clamp(this->OrbitDistance, Settings.MinimumOrbitDistance, Settings.MaximumOrbitDistance);
}

const EditorCameraSettings &EditorCameraController::GetSettings() const noexcept
{
	return this->Settings;
}

EditorCameraInteraction EditorCameraController::Update(Camera &Camera, const EditorCameraNavigationInput &Input, const float32 DeltaSeconds,
													   const bool ViewportHovered, const bool ViewportFocused,
													   const bool ViewportWindowFocused, const bool CancelNavigation,
													   const bool PointerCapturedByUI, const bool KeyboardCapturedByUI)
{
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f)
		throw std::invalid_argument("Editor camera delta time must be finite and non-negative");
	if (!std::isfinite(Input.Pointer.DeltaX) || !std::isfinite(Input.Pointer.DeltaY) || !std::isfinite(Input.Pointer.ScrollY) ||
		!std::isfinite(Input.Movement.x) || !std::isfinite(Input.Movement.y) || !std::isfinite(Input.Movement.z))
		throw std::invalid_argument("Editor camera navigation input must be finite");

	EditorCameraInteraction Result;
	const bool RightMouseDown = Input.Pointer.RightMouseDown;
	const bool Looking =
		!CancelNavigation && ViewportWindowFocused && ViewportHovered && !PointerCapturedByUI && !Input.Alt && RightMouseDown;
	const bool Orbiting = !Looking && ViewportHovered && !PointerCapturedByUI && Input.Alt && Input.LeftMouseDown;
	const bool Panning = !Looking && ViewportHovered && !PointerCapturedByUI && Input.MiddleMouseDown;
	const bool KeyboardNavigationAvailable =
		!CancelNavigation && ViewportWindowFocused && (ViewportFocused || ViewportHovered) && (!KeyboardCapturedByUI || Looking);

	const float32 DeltaX = Input.Pointer.DeltaX;
	const float32 DeltaY = Input.Pointer.DeltaY;
	if (Orbiting)
	{
		this->Orbit(Camera, DeltaX, DeltaY);
		Result.ConsumedPointer = true;
	}
	else if (Panning)
	{
		this->Pan(Camera, DeltaX, DeltaY);
		Result.ConsumedPointer = true;
	}
	else if (Looking)
	{
		Camera.Yaw += DeltaX * this->Settings.LookSensitivity;
		Camera.Pitch = std::clamp(Camera.Pitch - DeltaY * this->Settings.LookSensitivity, -89.9f, 89.9f);
		Camera.UpdateCameraVectors();
		if (Input.Pointer.ScrollY != 0.0f)
		{
			this->FlySpeedScale = std::clamp(this->FlySpeedScale * std::pow(this->Settings.FlySpeedStep, Input.Pointer.ScrollY),
											 this->Settings.MinimumFlySpeedScale, this->Settings.MaximumFlySpeedScale);
		}
		Result.ConsumedPointer = true;
		Result.WantsRelativePointer = true;
	}

	if (KeyboardNavigationAvailable)
	{
		this->Fly(Camera, Input.Movement, Input.Fast, DeltaSeconds);
		Result.ConsumedKeyboard = true;
		this->HasOrbitPivot = false;
	}
	else
	{
		this->FlyVelocity = glm::vec3(0.0f);
	}

	if (!Looking && ViewportHovered && !PointerCapturedByUI && Input.Pointer.ScrollY != 0.0f)
	{
		this->Dolly(Camera, Input.Pointer.ScrollY);
		Result.ConsumedPointer = true;
	}
	return Result;
}

bool EditorCameraController::FocusSelection(const document::SceneDocument &Document, Camera &Camera)
{
	this->SelectedScratch.clear();
	Document.GetSelection().ResolveInto(Document.GetScene(), this->SelectedScratch);
	if (this->SelectedScratch.empty())
		return false;

	glm::vec3 Center(0.0f);
	this->PositionsScratch.clear();
	this->PositionsScratch.reserve(this->SelectedScratch.size());
	auto Access = Document.GetScene().Read();
	for (const world::ObjectHandle Object : this->SelectedScratch)
	{
		const world::ComponentHandle<components::CObjectTransformComponent> Transform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(Object);
		if (!Transform.IsValid())
			continue;
		const glm::vec3 Position = Access.Resolve(Transform).GetPosition();
		this->PositionsScratch.push_back(Position);
		Center += Position;
	}
	if (this->PositionsScratch.empty())
		return false;
	Center /= static_cast<float32>(this->PositionsScratch.size());
	float32 Radius = 0.5f;
	for (const glm::vec3 &Position : this->PositionsScratch)
		Radius = std::max(Radius, glm::distance(Position, Center));
	this->Focus(Camera, Center, Radius);
	return true;
}

void EditorCameraController::Focus(Camera &Camera, const glm::vec3 &Center, const float32 Radius)
{
	if (!std::isfinite(Center.x) || !std::isfinite(Center.y) || !std::isfinite(Center.z) || !IsFinitePositive(Radius))
		throw std::invalid_argument("Editor camera focus requires a finite center and positive radius");
	this->OrbitPivot = Center;
	this->OrbitDistance = std::clamp(Radius / std::tan(glm::radians(Camera.FOV) * 0.5f) * 1.25f, this->Settings.MinimumOrbitDistance,
									 this->Settings.MaximumOrbitDistance);
	Camera.Position = this->OrbitPivot - Camera.Front * this->OrbitDistance;
	this->HasOrbitPivot = true;
}

const glm::vec3 &EditorCameraController::GetOrbitPivot() const noexcept
{
	return this->OrbitPivot;
}

glm::vec3 EditorCameraController::GetPlacementPoint(const Camera &Camera) const noexcept
{
	return this->HasOrbitPivot ? this->OrbitPivot : Camera.Position + Camera.Front * this->OrbitDistance;
}

float32 EditorCameraController::GetOrbitDistance() const noexcept
{
	return this->OrbitDistance;
}

float32 EditorCameraController::GetFlySpeedScale() const noexcept
{
	return this->FlySpeedScale;
}

void EditorCameraController::ValidateSettings(const EditorCameraSettings &Settings) const
{
	if (!IsFinitePositive(Settings.LookSensitivity) || !IsFinitePositive(Settings.OrbitSensitivity) ||
		!IsFinitePositive(Settings.PanSensitivity) || !IsFinitePositive(Settings.DollySensitivity) ||
		!IsFinitePositive(Settings.FlySpeed) || !IsFinitePositive(Settings.FastMultiplier) || !IsFinitePositive(Settings.FlyAcceleration) ||
		!IsFinitePositive(Settings.FlyDeceleration) || !IsFinitePositive(Settings.FlySpeedStep) || Settings.FlySpeedStep == 1.0f ||
		!IsFinitePositive(Settings.MinimumFlySpeedScale) || !IsFinitePositive(Settings.MaximumFlySpeedScale) ||
		Settings.MinimumFlySpeedScale >= Settings.MaximumFlySpeedScale || !IsFinitePositive(Settings.MinimumOrbitDistance) ||
		!IsFinitePositive(Settings.MaximumOrbitDistance) || Settings.MinimumOrbitDistance >= Settings.MaximumOrbitDistance)
	{
		throw std::invalid_argument("Editor camera settings must contain finite positive values and a valid orbit-distance range");
	}
}

void EditorCameraController::EnsureOrbitPivot(const Camera &Camera)
{
	if (this->HasOrbitPivot)
		return;
	this->OrbitDistance = std::clamp(this->OrbitDistance, this->Settings.MinimumOrbitDistance, this->Settings.MaximumOrbitDistance);
	this->OrbitPivot = Camera.Position + Camera.Front * this->OrbitDistance;
	this->HasOrbitPivot = true;
}

void EditorCameraController::Orbit(Camera &Camera, const float32 DeltaX, const float32 DeltaY)
{
	this->EnsureOrbitPivot(Camera);
	Camera.Yaw += DeltaX * this->Settings.OrbitSensitivity;
	Camera.Pitch = std::clamp(Camera.Pitch - DeltaY * this->Settings.OrbitSensitivity, -89.9f, 89.9f);
	Camera.UpdateCameraVectors();
	Camera.Position = this->OrbitPivot - Camera.Front * this->OrbitDistance;
}

void EditorCameraController::Pan(Camera &Camera, const float32 DeltaX, const float32 DeltaY)
{
	this->EnsureOrbitPivot(Camera);
	const float32 DistanceScale = std::max(this->OrbitDistance, this->Settings.MinimumOrbitDistance);
	const glm::vec3 Translation = (-Camera.Right * DeltaX + Camera.Up * DeltaY) * (this->Settings.PanSensitivity * DistanceScale);
	Camera.Position += Translation;
	this->OrbitPivot += Translation;
}

void EditorCameraController::Dolly(Camera &Camera, const float32 Scroll)
{
	this->EnsureOrbitPivot(Camera);
	const float32 Multiplier = std::exp(-Scroll * this->Settings.DollySensitivity);
	this->OrbitDistance =
		std::clamp(this->OrbitDistance * Multiplier, this->Settings.MinimumOrbitDistance, this->Settings.MaximumOrbitDistance);
	Camera.Position = this->OrbitPivot - Camera.Front * this->OrbitDistance;
}

void EditorCameraController::Fly(Camera &Camera, const glm::vec3 &Movement, const bool Fast, const float32 DeltaSeconds)
{
	float32 Speed = this->Settings.FlySpeed * this->FlySpeedScale;
	if (Fast)
		Speed *= this->Settings.FastMultiplier;
	const glm::vec3 Translation = Camera.Right * Movement.x + Camera.WorldUp * Movement.y + Camera.Front * Movement.z;
	const bool HasTranslation = glm::dot(Translation, Translation) > 0.0f;
	const glm::vec3 TargetVelocity = HasTranslation ? glm::normalize(Translation) * Speed : glm::vec3(0.0f);
	const float32 Response = HasTranslation ? this->Settings.FlyAcceleration : this->Settings.FlyDeceleration;
	const float32 Decay = std::exp(-Response * DeltaSeconds);
	const glm::vec3 VelocityError = this->FlyVelocity - TargetVelocity;
	const glm::vec3 Displacement = TargetVelocity * DeltaSeconds + VelocityError * ((1.0f - Decay) / Response);
	this->FlyVelocity = TargetVelocity + VelocityError * Decay;
	if (!std::isfinite(Displacement.x) || !std::isfinite(Displacement.y) || !std::isfinite(Displacement.z) ||
		!std::isfinite(this->FlyVelocity.x) || !std::isfinite(this->FlyVelocity.y) || !std::isfinite(this->FlyVelocity.z))
	{
		this->FlyVelocity = glm::vec3(0.0f);
		throw std::overflow_error("Editor camera movement exceeded its finite range");
	}
	Camera.Position += Displacement;
}

} // namespace editor::viewport
