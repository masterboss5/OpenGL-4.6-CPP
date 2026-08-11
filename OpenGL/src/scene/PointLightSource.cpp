#include "PointLightSource.h"

PointLightSource::PointLightSource(const glm::vec3 &Position, const glm::vec3 &Ambient, const glm::vec3 &Diffuse, const glm::vec3 &Specular,
								   const float32 Constant, const float32 Linear, const float32 Quadratic, const bool CastShadows)
{
	this->Position = Position;
	this->Pad6 = 0.0f;
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
