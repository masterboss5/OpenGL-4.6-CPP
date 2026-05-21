#pragma once
#include <glm.hpp>

namespace renderer
{
	struct Vertex final
	{
		glm::vec3 position;
		glm::vec3 normal;
		glm::vec2 uv;
		glm::vec3 tanget;
		glm::vec3 bitTangent;
	};
}