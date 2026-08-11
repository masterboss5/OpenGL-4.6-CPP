#include "DirectionalLightSource.h"

DirectionalLightSource::DirectionalLightSource(const glm::vec3 &Direction, const glm::vec3 &Ambient, const glm::vec3 &Diffuse,
											   const glm::vec3 &Specular, const bool CastShadows)
{
	this->Direction = Direction;
	this->CastShadows = CastShadows ? 1.0f : 0.0f;
	this->Ambient = Ambient;
	this->Pad10 = 0.0f;
	this->Diffuse = Diffuse;
	this->Pad11 = 0.0f;
	this->Specular = Specular;
	this->Pad12 = 0.0f;
}
