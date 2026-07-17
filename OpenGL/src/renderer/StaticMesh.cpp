#include "StaticMesh.h"

#include <limits>
#include <stdexcept>

#include "src/pipeline/device/OpenGLRuntime.h"

namespace
{
	constexpr std::size_t PositionComponentCount = 3;
	constexpr std::size_t NormalComponentCount = 3;
	constexpr std::size_t TextureCoordinateComponentCount = 2;
}

StaticMesh::StaticMesh(
	const string& meshName,
	const Material& meshMaterial,
	const std::vector<float32>& meshVertices,
	const std::vector<uint32>& meshIndices,
	const std::vector<float32>& meshTextureCoordinates,
	const std::vector<float32>& meshNormals)
	: name(meshName),
	vertices(meshVertices),
	indices(meshIndices),
	material(meshMaterial),
	textureCoordinates(meshTextureCoordinates),
	normals(meshNormals),
	vertexDescriptor(
		{
			{ .bindingIndex = 0, .strideInBytes = sizeof(float32) * PositionComponentCount },
			{ .bindingIndex = 1, .strideInBytes = sizeof(float32) * NormalComponentCount },
			{ .bindingIndex = 2, .strideInBytes = sizeof(float32) * TextureCoordinateComponentCount }
		},
		{
			{ .semantic = "POSITION", .location = 0, .bindingIndex = 0, .dataType = renderer::VertexAttributeDataType::Float32, .componentCount = PositionComponentCount },
			{ .semantic = "NORMAL", .location = 1, .bindingIndex = 1, .dataType = renderer::VertexAttributeDataType::Float32, .componentCount = NormalComponentCount },
			{ .semantic = "TEXCOORD", .location = 2, .bindingIndex = 2, .dataType = renderer::VertexAttributeDataType::Float32, .componentCount = TextureCoordinateComponentCount }
		})
{
	this->validateSourceData();
	pipeline::device::requireOpenGL46Context();
	pipeline::device::throwPendingOpenGLErrors("StaticMesh construction precondition");

	try
	{
		glCreateVertexArrays(1, &this->vertexArray);
		glCreateBuffers(1, &this->vertexBuffer);
		glCreateBuffers(1, &this->normalBuffer);
		glCreateBuffers(1, &this->textureCoordinateBuffer);
		glCreateBuffers(1, &this->indexBuffer);
		glNamedBufferStorage(this->vertexBuffer, static_cast<GLsizeiptr>(this->vertices.size() * sizeof(float32)), this->vertices.data(), 0);
		glNamedBufferStorage(this->normalBuffer, static_cast<GLsizeiptr>(this->normals.size() * sizeof(float32)), this->normals.data(), 0);
		glNamedBufferStorage(this->textureCoordinateBuffer, static_cast<GLsizeiptr>(this->textureCoordinates.size() * sizeof(float32)), this->textureCoordinates.data(), 0);
		glNamedBufferStorage(this->indexBuffer, static_cast<GLsizeiptr>(this->indices.size() * sizeof(uint32)), this->indices.data(), 0);
		this->vertexDescriptor.applyToVertexArray(this->vertexArray);
		glVertexArrayVertexBuffer(this->vertexArray, 0, this->vertexBuffer, 0, static_cast<GLsizei>(sizeof(float32) * PositionComponentCount));
		glVertexArrayVertexBuffer(this->vertexArray, 1, this->normalBuffer, 0, static_cast<GLsizei>(sizeof(float32) * NormalComponentCount));
		glVertexArrayVertexBuffer(this->vertexArray, 2, this->textureCoordinateBuffer, 0, static_cast<GLsizei>(sizeof(float32) * TextureCoordinateComponentCount));
		glVertexArrayElementBuffer(this->vertexArray, this->indexBuffer);
		pipeline::device::throwPendingOpenGLErrors("StaticMesh OpenGL 4.6 setup");
	}
	catch (...)
	{
		this->release();
		throw;
	}
}

StaticMesh::~StaticMesh()
{
	this->release();
}

uint32 StaticMesh::getIndicesCount() const noexcept { return static_cast<uint32>(this->indices.size()); }
const std::vector<float32>& StaticMesh::getVertices() const noexcept { return this->vertices; }
const std::vector<uint32>& StaticMesh::getIndices() const noexcept { return this->indices; }
GLuint StaticMesh::getVAO() const noexcept { return this->vertexArray; }
GLuint StaticMesh::getVertexVBO() const noexcept { return this->vertexBuffer; }
GLuint StaticMesh::getEBO() const noexcept { return this->indexBuffer; }
GLuint StaticMesh::getUVVBO() const noexcept { return this->textureCoordinateBuffer; }
GLuint StaticMesh::getNormalVBO() const noexcept { return this->normalBuffer; }
const Material& StaticMesh::getMaterial() const noexcept { return this->material; }
const renderer::VertexDescriptor& StaticMesh::getVertexDescriptor() const noexcept { return this->vertexDescriptor; }

void StaticMesh::release() noexcept
{
	if (this->indexBuffer != 0) glDeleteBuffers(1, &this->indexBuffer);
	if (this->textureCoordinateBuffer != 0) glDeleteBuffers(1, &this->textureCoordinateBuffer);
	if (this->normalBuffer != 0) glDeleteBuffers(1, &this->normalBuffer);
	if (this->vertexBuffer != 0) glDeleteBuffers(1, &this->vertexBuffer);
	if (this->vertexArray != 0) glDeleteVertexArrays(1, &this->vertexArray);
	this->indexBuffer = 0;
	this->textureCoordinateBuffer = 0;
	this->normalBuffer = 0;
	this->vertexBuffer = 0;
	this->vertexArray = 0;
}

void StaticMesh::validateSourceData() const
{
	if (this->name.empty()) throw std::invalid_argument("StaticMesh requires a non-empty debug name");
	if (this->vertices.empty() || this->indices.empty() || this->textureCoordinates.empty() || this->normals.empty()) throw std::invalid_argument("StaticMesh requires positions, normals, texture coordinates, and indices");
	if (this->vertices.size() % PositionComponentCount != 0 || this->normals.size() % NormalComponentCount != 0 || this->textureCoordinates.size() % TextureCoordinateComponentCount != 0) throw std::invalid_argument("StaticMesh source attributes have an invalid component count");
	const std::size_t vertexCount = this->vertices.size() / PositionComponentCount;
	if (this->normals.size() / NormalComponentCount != vertexCount || this->textureCoordinates.size() / TextureCoordinateComponentCount != vertexCount) throw std::invalid_argument("StaticMesh source attributes must use identical vertex counts");
	if (vertexCount > static_cast<std::size_t>(std::numeric_limits<uint32>::max())) throw std::invalid_argument("StaticMesh vertex count exceeds the engine index range");
	for (const uint32 index : this->indices) if (index >= vertexCount) throw std::invalid_argument("StaticMesh index references a vertex outside the mesh");
}
