#include "Camera.h"

#include <ext/matrix_clip_space.hpp>

Camera::Camera(float32 Sensitivity, float32 FOV, float32 NearPlane, float32 FarPlane)
	: Yaw(0.0f), Pitch(0.0f), Sensitivity(Sensitivity), FOV(FOV), NearPlane(NearPlane), FarPlane(FarPlane)
{
	this->Position = glm::vec3(0.0f, 0.0f, 0.0f);
	this->Front = glm::vec3(0.0f, 0.0f, -1.0f);
	this->Up = glm::vec3(0.0f, 1.0f, 0.0f);
	this->Right = glm::vec3(1.0f, 0.0f, 0.0f);
	this->WorldUp = glm::vec3(0.0f, 1.0f, 0.0f);
	this->UpdateCameraVectors();
}

void Camera::Update(const core::input::InputSnapshot &Input, const float32 DeltaTime)
{
	if (Input.IsKeyDown(core::input::Key::W))
	{
		this->Position += this->Front * DeltaTime * 10.0f;
	}

	if (Input.IsKeyDown(core::input::Key::S))
	{
		this->Position -= this->Front * DeltaTime * 10.0f;
	}

	if (Input.IsKeyDown(core::input::Key::A))
	{
		this->Position -= this->Right * DeltaTime * 10.0f;
	}

	if (Input.IsKeyDown(core::input::Key::D))
	{
		this->Position += this->Right * DeltaTime * 10.0f;
	}

	if (Input.IsKeyDown(core::input::Key::Space))
	{
		this->Position += this->WorldUp * DeltaTime * 10.0f;
	}

	if (Input.IsKeyDown(core::input::Key::LeftShift))
	{
		this->Position -= this->WorldUp * DeltaTime * 10.0f;
	}

	const float32 XOffset = static_cast<float32>(Input.GetMouseDeltaX()) * this->Sensitivity;
	const float32 YOffset = static_cast<float32>(Input.GetMouseDeltaY()) * this->Sensitivity;
	this->Yaw += XOffset;
	this->Pitch -= YOffset;

	this->Pitch = std::min(this->Pitch, 89.0f);
	this->Pitch = std::max(this->Pitch, -89.0f);
	this->UpdateCameraVectors();
}

glm::mat4 Camera::GetViewMatrix() const
{
	return glm::lookAt(this->Position, this->Position + this->Front, this->Up);
}

glm::mat4 Camera::GetProjectionMatrix(const core::WindowExtent Extent) const
{
	// The renderer uses GL_ZERO_TO_ONE and reversed-Z.  Swapping near/far gives
	// near geometry a depth of one and moves precision toward the camera.
	return glm::perspectiveRH_ZO(glm::radians(this->FOV), Extent.AspectRatio(), this->FarPlane, this->NearPlane);
}

void Camera::UpdateCameraVectors()
{
	glm::vec3 CalculatedFront;
	CalculatedFront.x = cos(glm::radians(this->Yaw)) * cos(glm::radians(this->Pitch));
	CalculatedFront.y = sin(glm::radians(this->Pitch));
	CalculatedFront.z = sin(glm::radians(this->Yaw)) * cos(glm::radians(this->Pitch));
	this->Front = glm::normalize(CalculatedFront);
	this->Right = glm::normalize(glm::cross(this->Front, this->WorldUp));
	this->Up = glm::normalize(glm::cross(this->Right, this->Front));
}

float32 Camera::GetYaw() const
{
	return this->Yaw;
}

float32 Camera::GetPitch() const
{
	return this->Pitch;
}
