#pragma once
#include "Source/scene/LightShadowParameters.h"
#include "Source/types.h"

#include <glm.hpp>

struct alignas(16) ENGINE_API DirectionalLightSource
{
  public:
	glm::vec3 Direction;
	float32 CastShadows;

	//-----------16-------

	glm::vec3 Ambient;
	float32 Pad10;

	//------------32-------

	glm::vec3 Diffuse;
	float32 Pad11;

	//------------48-------

	glm::vec3 Specular;
	float32 Pad12;

	//------------64-------

	LightShadowParameters ShadowParameters;
	float32 AngularDiameterDegrees;
	float32 CascadeDistributionExponent;
	uint32 CascadeCount;

	DirectionalLightSource(const glm::vec3 &Direction, const glm::vec3 &Ambient, const glm::vec3 &Diffuse, const glm::vec3 &Specular,
						   bool CastShadows = true, const LightShadowParameters &ShadowParameters = {},
						   float32 AngularDiameterDegrees = 0.5357f, uint32 CascadeCount = 4, float32 CascadeDistributionExponent = 2.0f);
};

static_assert(std::is_trivially_copyable_v<DirectionalLightSource>, "SSBO element type must be trivially copyable");
