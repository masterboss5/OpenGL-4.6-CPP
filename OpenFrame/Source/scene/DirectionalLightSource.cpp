#include "DirectionalLightSource.h"

DirectionalLightSource::DirectionalLightSource(const glm::vec3 &Direction, const glm::vec3 &Ambient, const glm::vec3 &Diffuse,
											   const glm::vec3 &Specular, const bool CastShadows,
											   const LightShadowParameters &ShadowParameters, const float32 AngularDiameterDegrees,
											   const uint32 CascadeCount, const float32 CascadeDistributionExponent)
{
	this->Direction = Direction;
	this->CastShadows = CastShadows ? 1.0f : 0.0f;
	this->Ambient = Ambient;
	this->Pad10 = 0.0f;
	this->Diffuse = Diffuse;
	this->Pad11 = 0.0f;
	this->Specular = Specular;
	this->Pad12 = 0.0f;
	this->ShadowParameters = ShadowParameters;
	this->AngularDiameterDegrees = AngularDiameterDegrees;
	this->CascadeDistributionExponent = CascadeDistributionExponent;
	this->CascadeCount = CascadeCount;
}
