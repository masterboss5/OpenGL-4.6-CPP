#include "PointLightSource.h"

PointLightSource::PointLightSource(
	const glm::vec3& position,
	const glm::vec3& ambient,
	const glm::vec3& diffuse,
	const glm::vec3& specular,
	const float constant,
	const float linear,
	const float quadratic
) {
	this->position = position;
	this->ambient = ambient;
	this->diffuse = diffuse;
	this->specular = specular;
	this->constant = constant;
	this->linear = linear;
	this->quadratic = quadratic;
}