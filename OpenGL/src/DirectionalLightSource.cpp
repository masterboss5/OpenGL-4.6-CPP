#include "DirectionalLightSource.h"

DirectionalLightSource::DirectionalLightSource(
	const glm::vec3& direction,
	const glm::vec3& ambient,
	const glm::vec3& diffuse,
	const glm::vec3& specular
) {
	this->direction = direction;
	this->ambient = ambient;
	this->diffuse = diffuse;
	this->specular = specular;
}
