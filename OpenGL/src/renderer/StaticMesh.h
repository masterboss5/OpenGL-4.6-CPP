#pragma once
#include <string>
#include <vector>
#include "PBRMaterial.h"

class StaticMesh final {
private:
	unsigned int VAO = 0;
	unsigned int vertexVBO = 0;
	unsigned int UVVBO = 0;
	unsigned int normalVBO = 0;
	unsigned int EBO = 0;
	const std::string name;
	const std::vector<float> vertices;
	const std::vector<unsigned int> indices;
	const Material material;
	const std::vector<float> uv;
	const std::vector<float> normals;
public:
	StaticMesh(
		const std::string& name,
		const Material& material,
		const std::vector<float>& vertices,
		const std::vector<unsigned int>& indices,
		const std::vector<float>& uv,
		const std::vector<float>& normals
	);

	unsigned int getIndicesCount() const;
	const std::vector<float>& getVertices() const;
	const std::vector<unsigned int>& getIndices() const;
	unsigned int getVAO() const;
	unsigned int getVertexVBO() const;
	unsigned int getEBO() const;
	unsigned int getUVVBO() const;
	unsigned int getNormalVBO() const;
	const Material& getMaterial() const;
};