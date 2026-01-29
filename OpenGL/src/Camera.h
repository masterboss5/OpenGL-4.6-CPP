#pragma once
#include <glm.hpp>
#include "Window.h"
#include <GLFW/glfw3.h>
#include <gtc/matrix_transform.hpp>
#include <algorithm>

class Camera final
{
public:
	glm::vec3 position;
	glm::vec3 front;
	glm::vec3 up;
	glm::vec3 right;
	glm::vec3 worldUp;
public:
	float yaw;
	float pitch;
	float sensitivity;
	float FOV;
	float nearPlane;
	float farPlane;

	Camera(float sensitivity, float FOV, float nearPlane, float farPlane);

	void update(const Window& window, float deltaTime);
	glm::mat4 getViewMatrix() const;
	glm::mat4 getProjectionMatrix(const Window& window) const;
	void updateCameraVectors();
	float getYaw() const;
	float getPitch() const;
};