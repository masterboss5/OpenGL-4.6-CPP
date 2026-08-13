#pragma once
#include "Source/core/input/InputTypes.h"
#include "Source/core/window/WindowTypes.h"
#include "Source/types.h"

#include <algorithm>
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>

enum class CameraProjectionMode : uint8
{
	Perspective,
	Orthographic
};

class ENGINE_API Camera final
{
  public:
	glm::vec3 Position;
	glm::vec3 Front;
	glm::vec3 Up;
	glm::vec3 Right;
	glm::vec3 WorldUp;

  public:
	float32 Yaw;
	float32 Pitch;
	float32 Sensitivity;
	float32 FOV;
	float32 NearPlane;
	float32 FarPlane;
	CameraProjectionMode Projection = CameraProjectionMode::Perspective;
	float32 OrthographicHeight = 10.0f;
	float32 ExposureCompensation = 0.0f;
	bool TemporalJitterEnabled = true;

	Camera(float32 Sensitivity, float32 FOV, float32 NearPlane, float32 FarPlane);

	void Update(const core::input::InputSnapshot &Input, float32 DeltaTime);
	[[nodiscard]] glm::mat4 GetViewMatrix() const;
	[[nodiscard]] glm::mat4 GetProjectionMatrix(core::WindowExtent Extent) const;
	void UpdateCameraVectors();
	[[nodiscard]] float32 GetYaw() const;
	[[nodiscard]] float32 GetPitch() const;
};
