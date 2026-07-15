#include "VertexBuffer.h"

renderer::VertexBuffer::VertexBuffer()
{
	glGenBuffers(1, &this->ID);
	this->bind();
}

renderer::VertexBuffer::~VertexBuffer()
{
	glDeleteBuffers(1, &this->ID);
}

renderer::VertexBuffer::VertexBuffer(VertexBuffer&&)
{
}


renderer::VertexBuffer& renderer::VertexBuffer::operator=(VertexBuffer&&)
{
	// TODO: insert return statement here
}

void renderer::VertexBuffer::bind()
{
	glBindBuffer(GL_VERTEX_ARRAY, this->ID);
}

GLuint renderer::VertexBuffer::getID()
{
	return this->ID;
}
