#include "SpotLightSource.h"

SpotLightSource::SpotLightSource(
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
) {
	this->position = position;
	this->direction = direction;
	this->cutOff = cutOff;
	this->outerCutOff = outerCutOff;
	this->ambient = ambient;
	this->diffuse = diffuse;
	this->specular = specular;
	this->constant = constant;
	this->linear = linear;
	this->quadratic = quadratic;
}

void SpotLightSource::lookAt(const glm::vec3& target)
{
	this->direction = glm::normalize(target - this->position);
}
