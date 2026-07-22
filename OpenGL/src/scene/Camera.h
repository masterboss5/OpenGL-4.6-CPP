#pragma once
#include "src/core/input/InputTypes.h"
#include "src/core/window/WindowTypes.h"
#include "src/types.h"

#include <algorithm>
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>

class Camera final
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

	Camera(float32 Sensitivity, float32 FOV, float32 NearPlane, float32 FarPlane);

	void Update(const core::input::InputSnapshot &Input, float32 DeltaTime);
	[[nodiscard]] glm::mat4 GetViewMatrix() const;
	[[nodiscard]] glm::mat4 GetProjectionMatrix(core::WindowExtent Extent) const;
	void UpdateCameraVectors();
	[[nodiscard]] float32 GetYaw() const;
	[[nodiscard]] float32 GetPitch() const;
};
