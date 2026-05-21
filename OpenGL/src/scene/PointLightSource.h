#pragma once
#include <glm.hpp>

struct alignas(16) PointLightSource {
public:
	glm::vec3 position;
	float pad6;

	//-----------16-------

	glm::vec3 ambient;
	float pad10;

	//------------32-------

	glm::vec3 diffuse;
	float pad11;

	//------------48-------

	glm::vec3 specular;
	float pad12;

	//------------64-------

	float constant;
	float linear;
	float quadratic;
	float pad13;

	//------------80-------

	PointLightSource(
		const glm::vec3& position,
		const glm::vec3& ambient,
		const glm::vec3& diffuse,
		const glm::vec3& specular,
		const float constant,
		const float linear,
		const float quadratic
	);
};

static_assert(std::is_trivially_copyable_v<PointLightSource>, "SSBO element type must be trivially opyable");