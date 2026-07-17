#pragma once

#include <string>
#include <vector>

#include <GL/glew.h>

#include "PBRMaterial.h"
#include "src/pipeline/vertex/VertexDescriptor.h"
#include "src/types.h"

// Immutable indexed mesh with a fixed engine vertex contract. StaticMesh owns
// its VAO and all backing buffers; it cannot be copied because OpenGL object
// names do not imply shared ownership.
class StaticMesh final
{
public:
	StaticMesh(
		const string& name,
		const Material& material,
		const std::vector<float32>& vertices,
		const std::vector<uint32>& indices,
		const std::vector<float32>& uv,
		const std::vector<float32>& normals);
	~StaticMesh();

	StaticMesh(const StaticMesh&) = delete;
	StaticMesh& operator=(const StaticMesh&) = delete;
	StaticMesh(StaticMesh&&) = delete;
	StaticMesh& operator=(StaticMesh&&) = delete;

	[[nodiscard]] uint32 getIndicesCount() const noexcept;
	[[nodiscard]] const std::vector<float32>& getVertices() const noexcept;
	[[nodiscard]] const std::vector<uint32>& getIndices() const noexcept;
	[[nodiscard]] GLuint getVAO() const noexcept;
	[[nodiscard]] GLuint getVertexVBO() const noexcept;
	[[nodiscard]] GLuint getEBO() const noexcept;
	[[nodiscard]] GLuint getUVVBO() const noexcept;
	[[nodiscard]] GLuint getNormalVBO() const noexcept;
	[[nodiscard]] const Material& getMaterial() const noexcept;
	[[nodiscard]] const renderer::VertexDescriptor& getVertexDescriptor() const noexcept;

private:
	GLuint vertexArray = 0;
	GLuint vertexBuffer = 0;
	GLuint textureCoordinateBuffer = 0;
	GLuint normalBuffer = 0;
	GLuint indexBuffer = 0;
	string name;
	std::vector<float32> vertices;
	std::vector<uint32> indices;
	Material material;
	std::vector<float32> textureCoordinates;
	std::vector<float32> normals;
	renderer::VertexDescriptor vertexDescriptor;

	void release() noexcept;
	void validateSourceData() const;
};
