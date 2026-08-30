#pragma once

#include "Source/core/input/InputTypes.h"
#include "Source/editor/document/SceneDocument.h"
#include "Source/scene/Camera.h"
#include "Source/types.h"

#include <vector>

namespace editor::viewport
{
struct EditorCameraSettings final
{
	float32 LookSensitivity = 0.12f;
	float32 OrbitSensitivity = 0.12f;
	float32 PanSensitivity = 0.0015f;
	float32 DollySensitivity = 0.12f;
	float32 FlySpeed = 10.0f;
	float32 FastMultiplier = 4.0f;
	float32 FlyAcceleration = 64.0f;
	float32 FlyDeceleration = 80.0f;
	float32 FlySpeedStep = 1.25f;
	float32 MinimumFlySpeedScale = 0.125f;
	float32 MaximumFlySpeedScale = 64.0f;
	float32 MinimumOrbitDistance = 0.05f;
	float32 MaximumOrbitDistance = 1'000'000.0f;
};

struct EditorCameraInteraction final
{
	bool ConsumedPointer = false;
	bool ConsumedKeyboard = false;
	bool WantsRelativePointer = false;
};

struct EditorCameraPointerInput final
{
	float32 DeltaX = 0.0f;
	float32 DeltaY = 0.0f;
	float32 ScrollY = 0.0f;
	bool RightMouseDown = false;
};

struct EditorCameraNavigationInput final
{
	EditorCameraPointerInput Pointer;
	glm::vec3 Movement{0.0f};
	bool Fast = false;
	bool Alt = false;
	bool LeftMouseDown = false;
	bool MiddleMouseDown = false;
};

class EditorCameraController final
{
  public:
	void SetSettings(const EditorCameraSettings &Settings);
	[[nodiscard]] const EditorCameraSettings &GetSettings() const noexcept;

	[[nodiscard]] EditorCameraInteraction Update(Camera &Camera, const EditorCameraNavigationInput &Input, float32 DeltaSeconds,
												 bool ViewportHovered, bool ViewportFocused, bool ViewportWindowFocused,
												 bool CancelNavigation, bool PointerCapturedByUI, bool KeyboardCapturedByUI);
	[[nodiscard]] bool FocusSelection(const document::SceneDocument &Document, Camera &Camera);
	void Focus(Camera &Camera, const glm::vec3 &Center, float32 Radius);

	[[nodiscard]] const glm::vec3 &GetOrbitPivot() const noexcept;
	[[nodiscard]] glm::vec3 GetPlacementPoint(const Camera &Camera) const noexcept;
	[[nodiscard]] float32 GetOrbitDistance() const noexcept;
	[[nodiscard]] float32 GetFlySpeedScale() const noexcept;

  private:
	void ValidateSettings(const EditorCameraSettings &Settings) const;
	void EnsureOrbitPivot(const Camera &Camera);
	void Orbit(Camera &Camera, float32 DeltaX, float32 DeltaY);
	void Pan(Camera &Camera, float32 DeltaX, float32 DeltaY);
	void Dolly(Camera &Camera, float32 Scroll);
	void Fly(Camera &Camera, const glm::vec3 &Movement, bool Fast, float32 DeltaSeconds);

	EditorCameraSettings Settings;
	glm::vec3 OrbitPivot{0.0f};
	float32 OrbitDistance = 10.0f;
	float32 FlySpeedScale = 1.0f;
	glm::vec3 FlyVelocity{0.0f};
	bool HasOrbitPivot = false;
	std::vector<world::ObjectHandle> SelectedScratch;
	std::vector<glm::vec3> PositionsScratch;
};
} // namespace editor::viewport
