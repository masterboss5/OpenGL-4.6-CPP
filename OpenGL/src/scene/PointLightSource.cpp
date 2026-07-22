#include "PointLightSource.h"

PointLightSource::PointLightSource(const glm::vec3 &Position, const glm::vec3 &Ambient, const glm::vec3 &Diffuse, const glm::vec3 &Specular,
								   const float32 Constant, const float32 Linear, const float32 Quadratic)
{
	this->Position = Position;
	this->Ambient = Ambient;
	this->Diffuse = Diffuse;
	this->Specular = Specular;
	this->Constant = Constant;
	this->Linear = Linear;
	this->Quadratic = Quadratic;
}
