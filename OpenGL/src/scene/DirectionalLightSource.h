#pragma once
#include "src/types.h"

#include <glm.hpp>

struct alignas(16) DirectionalLightSource
{
  public:
	glm::vec3 Direction;
	float32 Pad14;

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

	DirectionalLightSource(const glm::vec3 &Direction, const glm::vec3 &Ambient, const glm::vec3 &Diffuse, const glm::vec3 &Specular);
};

static_assert(std::is_trivially_copyable_v<DirectionalLightSource>, "SSBO element type must be trivially copyable");
