#include "SpotLightSource.h"

SpotLightSource::SpotLightSource(const glm::vec3 &Position, const glm::vec3 &Direction, const float32 CutOff, const float32 OuterCutOff,
								 const glm::vec3 &Ambient, const glm::vec3 &Diffuse, const glm::vec3 &Specular, const float32 Constant,
								 const float32 Linear, const float32 Quadratic, const bool CastShadows)
{
	this->Position = Position;
	this->Pad6 = 0.0f;
	this->Direction = Direction;
	this->Pad7 = 0.0f;
	this->CutOff = CutOff;
	this->OuterCutOff = OuterCutOff;
	this->Pad8 = 0;
	this->Pad9 = 0;
	this->Ambient = Ambient;
	this->Pad10 = 0.0f;
	this->Diffuse = Diffuse;
	this->Pad11 = 0.0f;
	this->Specular = Specular;
	this->Pad12 = 0.0f;
	this->Constant = Constant;
	this->Linear = Linear;
	this->Quadratic = Quadratic;
	this->CastShadows = CastShadows ? 1.0f : 0.0f;
}

void SpotLightSource::LookAt(const glm::vec3 &Target)
{
	this->Direction = glm::normalize(Target - this->Position);
}
