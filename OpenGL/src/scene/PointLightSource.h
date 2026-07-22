#pragma once
#include "src/types.h"

#include <glm.hpp>

struct alignas(16) PointLightSource
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
	float32 Pad13;

	//------------80-------

	PointLightSource(const glm::vec3 &Position, const glm::vec3 &Ambient, const glm::vec3 &Diffuse, const glm::vec3 &Specular,
					 float32 Constant, float32 Linear, float32 Quadratic);
};

static_assert(std::is_trivially_copyable_v<PointLightSource>, "SSBO element type must be trivially opyable");
