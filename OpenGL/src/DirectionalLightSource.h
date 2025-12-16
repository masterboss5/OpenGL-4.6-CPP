#pragma once
#include <glm.hpp>

struct alignas(16) DirectionalLightSource {
public:
	glm::vec3 direction;
	float pad14;

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

	DirectionalLightSource(
		const glm::vec3& direction,
		const glm::vec3& ambient,
		const glm::vec3& diffuse,
		const glm::vec3& specular
	);
};