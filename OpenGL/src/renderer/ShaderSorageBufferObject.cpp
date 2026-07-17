#include "ShaderStorageBufferObject.h"

template<typename T, BindingPoint BINDING>
ShaderSorageBufferObject<T, BINDING>::ShaderSorageBufferObject(size_t maxElements)
	: maxElements(maxElements),
	bufferPointer(nullptr)
{
	this->bytesSize = sizeof(T) * maxElements;


	glGenBuffers(1, &this->bufferID);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, this->bufferID);


	//glBufferData(GL_SHADER_STORAGE_BUFFER, this->bytesSize, nullptr, GL_DYNAMIC_DRAW);

	glBufferStorage(GL_SHADER_STORAGE_BUFFER, this->bytesSize, nullptr, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);


	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(BINDING), this->bufferID);
	//this->bufferPointer = static_cast<T*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, this->bytesSize, 
	//	GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));

	this->bufferPointer = static_cast<T*> (glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, this->bytesSize, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));

	if (this->bufferPointer == nullptr)
	{
		LOG_ERROR("Null pointer for map buffer range");
	}
}

template<typename T, BindingPoint BINDING>
ShaderSorageBufferObject<T, BINDING>::~ShaderSorageBufferObject()
{
	this->bindBuffer();
	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
	glDeleteBuffers(1, &this->bufferID);
}

template<typename T, BindingPoint BINDING>
void ShaderSorageBufferObject<T, BINDING>::upload(const T* data, size_t count) const
{
	assert(count <= this->maxElements);

	if (this->bufferPointer == nullptr)
	{
		LOG_ERROR("Null pointer for map buffer range");
	}

	std::memcpy(this->bufferPointer, data, sizeof(T) * count);
}

template<typename T, BindingPoint BINDING>
GLuint ShaderSorageBufferObject<T, BINDING>::getBufferID() const
{
	return this->bufferID;
}

template<typename T, BindingPoint BINDING>
void ShaderSorageBufferObject<T, BINDING>::bindBuffer() const
{
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, this->bufferID);
}

template<typename T, BindingPoint BINDING>
size_t ShaderSorageBufferObject<T, BINDING>::getBytesSize() const
{
	return this->bytesSize;
}

template<typename T, BindingPoint BINDING>
size_t ShaderSorageBufferObject<T, BINDING>::getMaxElements() const
{
	return this->maxElements;
}

template<typename T, BindingPoint BINDING>
T* ShaderSorageBufferObject<T, BINDING>::getBufferPointer() const
{
	return this->bufferPointer;
}

template class ShaderSorageBufferObject<PointLightSource, BindingPoint::POINT_LIGHT_SOURCE>;
template class ShaderSorageBufferObject<DirectionalLightSource, BindingPoint::DIRECTIONAL_LIGHT_SOURCES>;
template class ShaderSorageBufferObject<SpotLightSource, BindingPoint::SPOT_LIGHT_SOURCES>;
