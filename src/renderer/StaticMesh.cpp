#include "StaticMesh.h"

StaticMesh::StaticMesh(const std::string& name,
	const Material& material,
	const std::vector<float>& vertices,
	const std::vector<unsigned int>& indices,
	const std::vector<float>& uv,
	const std::vector<float>& normals
) : material(material), vertices(vertices), indices(indices), uv(uv), normals(normals)
{
	//VAO
	glGenVertexArrays(1, &this->VAO);
	glBindVertexArray(this->VAO);

	//Position binding 0
	glGenBuffers(1,   &this->vertexVBO);
	glBindBuffer(GL_ARRAY_BUFFER, this->vertexVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, nullptr);
	glEnableVertexAttribArray(0);

	//Normal binding 1
	glGenBuffers(1, &this->normalVBO);
	glBindBuffer(GL_ARRAY_BUFFER, this->normalVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * normals.size(), normals.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, nullptr);
	glEnableVertexAttribArray(1);

	//UV binding 2
	glGenBuffers(1, &this->UVVBO);
	glBindBuffer(GL_ARRAY_BUFFER, this->UVVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * uv.size(), uv.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);
	glEnableVertexAttribArray(2);

	//EBO
	glGenBuffers(1, &this->EBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->EBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * indices.size(), indices.data(), GL_STATIC_DRAW);
};

unsigned int StaticMesh::getVAO() const {
	return this->VAO;
}

unsigned int StaticMesh::getVertexVBO() const {
	return this->vertexVBO;
}

unsigned int StaticMesh::getEBO() const {
	return this->EBO;
}

unsigned int StaticMesh::getIndicesCount() const {
	return this->indices.size();
}

const std::vector<float>& StaticMesh::getVertices() const {
	return this->vertices;
}

const std::vector<unsigned int>& StaticMesh::getIndices() const {
	return this->indices;
}

unsigned int StaticMesh::getUVVBO() const {
	return this->UVVBO;
}

unsigned int StaticMesh::getNormalVBO() const {
	return this->normalVBO;
}

const Material& StaticMesh::getMaterial() const {
	return this->material;
}