#include "DirectionalLightSource.h"

DirectionalLightSource::DirectionalLightSource(const glm::vec3 &Direction, const glm::vec3 &Ambient, const glm::vec3 &Diffuse,
											   const glm::vec3 &Specular)
{
	this->Direction = Direction;
	this->Ambient = Ambient;
	this->Diffuse = Diffuse;
	this->Specular = Specular;
}
