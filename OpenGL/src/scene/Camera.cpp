#include "Camera.h"
#include <iostream>
#include <ext/matrix_clip_space.hpp>
#include "src/core/input/InputManager.h"

Camera::Camera(float sensitivity, float FOV, float nearPlane, float farPlane) : yaw(0.0f), pitch(0.0f), sensitivity(sensitivity), FOV(FOV), nearPlane(nearPlane), farPlane(farPlane)
{
	this->position = glm::vec3(0.0f, 0.0f, 0.0f);
	this->front = glm::vec3(0.0f, 0.0f, -1.0f);
	this->up = glm::vec3(0.0f, 1.0f, 0.0f);
	this->right = glm::vec3(1.0f, 0.0f, 0.0f);
	this->worldUp = glm::vec3(0.0f, 1.0f, 0.0f);
	this->updateCameraVectors();
}

void Camera::update(const Window& window, float deltaTime)
{
	//if (window.isKeyPressed(GLFW_KEY_W)) {
	//	this->position += this->front * deltaTime * 10.0f;
	//}

	//if (window.isKeyPressed(GLFW_KEY_S)) {
	//	this->position -= this->front * deltaTime * 10.0f;
	//}

	//if (window.isKeyPressed(GLFW_KEY_A)) {
	//	this->position -= this->right * deltaTime * 10.0f;
	//}

	//if (window.isKeyPressed(GLFW_KEY_D)) {
	//	this->position += this->right * deltaTime * 10.0f;
	//}

	//if (window.isKeyPressed(GLFW_KEY_SPACE)) {
	//	this->position += this->worldUp * deltaTime * 10.0f;
	//}

	//if (window.isKeyPressed(GLFW_KEY_LEFT_SHIFT)) {
	//	this->position -= this->worldUp * deltaTime * 10.0f;
	//}

	if (core::input::InputManager::getInstance()->isKeyPressed(GLFW_KEY_W)) {
		this->position += this->front * deltaTime * 10.0f;
	}

	if (core::input::InputManager::getInstance()->isKeyPressed(GLFW_KEY_S)) {
		this->position -= this->front * deltaTime * 10.0f;
	}

	if (core::input::InputManager::getInstance()->isKeyPressed(GLFW_KEY_A)) {
		this->position -= this->right * deltaTime * 10.0f;
	}

	if (core::input::InputManager::getInstance()->isKeyPressed(GLFW_KEY_D)) {
		this->position += this->right * deltaTime * 10.0f;
	}

	if (core::input::InputManager::getInstance()->isKeyPressed(GLFW_KEY_SPACE)) {
		this->position += this->worldUp * deltaTime * 10.0f;
	}

	if (core::input::InputManager::getInstance()->isKeyPressed(GLFW_KEY_LEFT_SHIFT)) {
		this->position -= this->worldUp * deltaTime * 10.0f;
	}

	float xOffset = core::input::InputManager::getInstance()->getDeltaMouseX() * this->sensitivity;
	float yOffset = core::input::InputManager::getInstance()->getDeltaMouseY() * this->sensitivity;
	this->yaw += xOffset;
	this->pitch -= yOffset;

	this->pitch = std::min(this->pitch, 89.0f);
	this->pitch = std::max(this->pitch, -89.0f);
	this->updateCameraVectors();
}

glm::mat4 Camera::getViewMatrix() const
{
	return glm::lookAt(this->position, this->position + this->front, this->up);
}

glm::mat4 Camera::getProjectionMatrix(const Window& window) const
{
	// The renderer uses GL_ZERO_TO_ONE and reversed-Z.  Swapping near/far gives
	// near geometry a depth of one and moves precision toward the camera.
	return glm::perspectiveRH_ZO(glm::radians(this->FOV), window.getAspectRatio(), this->farPlane, this->nearPlane);
}

void Camera::updateCameraVectors()
{
	glm::vec3 front;
	front.x = cos(glm::radians(this->yaw)) * cos(glm::radians(this->pitch));
	front.y = sin(glm::radians(this->pitch));
	front.z = sin(glm::radians(this->yaw)) * cos(glm::radians(this->pitch));
	this->front = glm::normalize(front);
	this->right = glm::normalize(glm::cross(this->front, this->worldUp));
	this->up = glm::normalize(glm::cross(this->right, this->front));
}

float Camera::getYaw() const
{
	return this->yaw;
}

float Camera::getPitch() const
{
	return this->pitch;
}
