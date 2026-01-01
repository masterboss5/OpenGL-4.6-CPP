#include "ShaderStorageBufferObject.h"

template<typename T, BindingPoint BINDING>
ShaderSorageBufferObject<T, BINDING>::ShaderSorageBufferObject(size_t maxElements)
	: maxElements(maxElements)
{
	this->bytesSize = sizeof(T) * maxElements;
	glGenBuffers(1, &this->bufferID);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING, this->bufferID)
}

template<typename T, BindingPoint BINDING>
ShaderSorageBufferObject<T, BINDING>::~ShaderSorageBufferObject()
{
	glDeleteBuffers(1, &this->bufferID);
}

template<typename T, BindingPoint BINDING>
GLuint ShaderSorageBufferObject<T, BINDING>::getBufferID() const
{
	return this->bufferID;
}

template<typename T, BindingPoint BINDING>
void ShaderSorageBufferObject<T, BINDING>::bindBuffer() const
{
	glGenBuffers(1, &this->bufferID);
}

template<typename T, BindingPoint BINDING>
GLuint ShaderSorageBufferObject<T, BINDING>::getBytesSize() const
{
	return this->bytesSize;
}

template<typename T, BindingPoint BINDING>
GLuint ShaderSorageBufferObject<T, BINDING>::getMaxElements() const
{
	return this->maxElements;
}
