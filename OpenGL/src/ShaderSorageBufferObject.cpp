#include "ShaderStorageBufferObject.h"

template<typename T, BindingPoint BINDING, GLenum BUFFER_USAGE>
ShaderSorageBufferObject<T, BINDING, BUFFER_USAGE>::ShaderSorageBufferObject(size_t maxElements)
	: maxElements(maxElements)
{
	this->bytesSize = sizeof(T) * maxElements;
	glGenBuffers(1, &this->bufferID);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, this->bufferID);
	glBufferData(GL_SHADER_STORAGE_BUFFER, this->bytesSize, nullptr, BUFFER_USAGE);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING, this->bufferID);
	this->bufferPointer = static_cast<T*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, this->bytesSize, 
		GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
}

template<typename T, BindingPoint BINDING, GLenum BUFFER_USAGE>
ShaderSorageBufferObject<T, BINDING, BUFFER_USAGE>::~ShaderSorageBufferObject()
{
	this->bindBuffer();
	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
	glDeleteBuffers(1, &this->bufferID);
}

template<typename T, BindingPoint BINDING, GLenum BUFFER_USAGE>
void ShaderSorageBufferObject<T, BINDING, BUFFER_USAGE>::uploadData(const T* data, size_t count) const
{
	assert(count <= this->maxElements);
	std::memcpy(this->bufferPointer, data, sizeof(T) * count);
}

template<typename T, BindingPoint BINDING, GLenum BUFFER_USAGE>
GLuint ShaderSorageBufferObject<T, BINDING, BUFFER_USAGE>::getBufferID() const
{
	return this->bufferID;
}

template<typename T, BindingPoint BINDING, GLenum BUFFER_USAGE>
void ShaderSorageBufferObject<T, BINDING, BUFFER_USAGE>::bindBuffer() const
{
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, this->bufferID);
}

template<typename T, BindingPoint BINDING, GLenum BUFFER_USAGE>
size_t ShaderSorageBufferObject<T, BINDING, BUFFER_USAGE>::getBytesSize() const
{
	return this->bytesSize;
}

template<typename T, BindingPoint BINDING, GLenum BUFFER_USAGE>
size_t ShaderSorageBufferObject<T, BINDING, BUFFER_USAGE>::getMaxElements() const
{
	return this->maxElements;
}

template<typename T, BindingPoint BINDING, GLenum BUFFER_USAGE>
T* ShaderSorageBufferObject<T, BINDING, BUFFER_USAGE>::getBufferPointer() const
{
	return this->bufferPointer;
}
