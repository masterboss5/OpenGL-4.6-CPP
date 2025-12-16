#pragma once
#include <glm.hpp>


struct alignas(16) SpotLightSource {
public:
	glm::vec3 position;
    float pad6;

	//-----------16-------

    glm::vec3 direction;
    float pad7;

    //-----------32-------

    float cutOff;
    float outerCutOff;
    int pad8;
    int pad9;

	//-----------48-------

    glm::vec3 ambient;
    float pad10;

	//------------64-------

    glm::vec3 diffuse;
    float pad11;

	//------------80-------

    glm::vec3 specular;
    float pad12;

	//------------96-------

    float constant;
    float linear;
    float quadratic;
    float pad13;

	//------------112-------

	SpotLightSource(
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

	void lookAt(const glm::vec3& target);
};