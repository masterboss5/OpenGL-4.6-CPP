#include "SpotLightSource.h"

SpotLightSource::SpotLightSource(const glm::vec3 &Position, const glm::vec3 &Direction, const float32 CutOff, const float32 OuterCutOff,
								 const glm::vec3 &Ambient, const glm::vec3 &Diffuse, const glm::vec3 &Specular, const float32 Constant,
								 const float32 Linear, const float32 Quadratic)
{
	this->Position = Position;
	this->Direction = Direction;
	this->CutOff = CutOff;
	this->OuterCutOff = OuterCutOff;
	this->Ambient = Ambient;
	this->Diffuse = Diffuse;
	this->Specular = Specular;
	this->Constant = Constant;
	this->Linear = Linear;
	this->Quadratic = Quadratic;
}

void SpotLightSource::LookAt(const glm::vec3 &Target)
{
	this->Direction = glm::normalize(Target - this->Position);
}
