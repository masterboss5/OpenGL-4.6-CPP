#pragma once
#include "src/resource/ResourceManager.h"
#include <vector>

namespace renderer
{
	class Mesh final
	{
	private:
		std::vector<Vertex> vertices;
		std::vector<unsigned int> indices;

	public:
		explicit Mesh(resource::ReadMeshContext& meshContext,
			resource::ReadPBRMaterialContext& materialContext);
	};
}