#pragma once
#include <glm.hpp>


struct alignas(16) LightSource {
public:
	enum class LightType : int {
		POINT = 0,
		SPOT = 1
	};

	LightType lightType; //4
	bool isActive; //1

	glm::vec3 position; //12
	glm::vec3 direction;//12
	float cutOff; //4
	float outerCutOff; //4

	glm::vec3 ambient; //12
	glm::vec3 diffuse; //12
	glm::vec3 specular; //12

	float constant; //4
	float linear; //4
	float quadratic; //4

	LightSource(
		const LightType lightType,
		const glm::vec3& position,
		const glm::vec3& direction,
		const float cutOff,
		const float outerCutOff,
		const glm::vec3& ambient,
		const glm::vec3& diffuse,
		const glm::vec3& specular,
		const float constant,
		const float linear,
		const float quadratic
	);


};