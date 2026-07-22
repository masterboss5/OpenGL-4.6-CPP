#pragma once
#include "src/types.h"

#include <glm.hpp>

struct alignas(16) SpotLightSource
{
  public:
	glm::vec3 Position;
	float32 Pad6;

	//-----------16-------

	glm::vec3 Direction;
	float32 Pad7;

	//-----------32-------

	float32 CutOff;
	float32 OuterCutOff;
	int32 Pad8;
	int32 Pad9;

	//-----------48-------

	glm::vec3 Ambient;
	float32 Pad10;

	//------------64-------

	glm::vec3 Diffuse;
	float32 Pad11;

	//------------80-------

	glm::vec3 Specular;
	float32 Pad12;

	//------------96-------

	float32 Constant;
	float32 Linear;
	float32 Quadratic;
	float32 Pad13;

	//------------112-------

	SpotLightSource(const glm::vec3 &Position, const glm::vec3 &Direction, float32 CutOff, float32 OuterCutOff, const glm::vec3 &Ambient,
					const glm::vec3 &Diffuse, const glm::vec3 &Specular, float32 Constant, float32 Linear, float32 Quadratic);

	void LookAt(const glm::vec3 &Target);
};

static_assert(std::is_trivially_copyable_v<SpotLightSource>, "SSBO element type must be trivially copyable");
