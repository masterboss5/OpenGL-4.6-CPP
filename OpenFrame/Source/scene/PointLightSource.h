#pragma once
#include "Source/scene/LightShadowParameters.h"
#include "Source/types.h"

#include <glm.hpp>

struct alignas(16) ENGINE_API PointLightSource
{
  public:
	glm::vec3 Position;
	float32 Pad6;

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

	float32 Constant;
	float32 Linear;
	float32 Quadratic;
	float32 CastShadows;

	//------------80-------

	LightShadowParameters ShadowParameters;
	uint32 ShadowPad0;
	uint32 ShadowPad1;
	uint32 ShadowPad2;

	PointLightSource(const glm::vec3 &Position, const glm::vec3 &Ambient, const glm::vec3 &Diffuse, const glm::vec3 &Specular,
					 float32 Constant, float32 Linear, float32 Quadratic, bool CastShadows = true,
					 const LightShadowParameters &ShadowParameters = {});
};

static_assert(std::is_trivially_copyable_v<PointLightSource>, "SSBO element type must be trivially opyable");
