#include "VertexBuffer.h"
#include "src/pipeline/device/OpenGLRuntime.h"

#include <limits>
#include <utility>

namespace renderer
{
	namespace
	{
		[[nodiscard]] constexpr GLbitfield toOpenGLFlags(VertexBufferStorage options)
		{
			return static_cast<GLbitfield>(options);
		}

		[[nodiscard]] constexpr GLbitfield toOpenGLFlags(VertexBufferMapAccess options)
		{
			return static_cast<GLbitfield>(options);
		}

		constexpr GLbitfield VALID_STORAGE_FLAGS =
			GL_DYNAMIC_STORAGE_BIT |
			GL_MAP_READ_BIT |
			GL_MAP_WRITE_BIT |
			GL_MAP_PERSISTENT_BIT |
			GL_MAP_COHERENT_BIT |
			GL_CLIENT_STORAGE_BIT;

		constexpr GLbitfield VALID_MAP_FLAGS =
			GL_MAP_READ_BIT |
			GL_MAP_WRITE_BIT |
			GL_MAP_INVALIDATE_RANGE_BIT |
			GL_MAP_INVALIDATE_BUFFER_BIT |
			GL_MAP_FLUSH_EXPLICIT_BIT |
			GL_MAP_UNSYNCHRONIZED_BIT |
			GL_MAP_PERSISTENT_BIT |
			GL_MAP_COHERENT_BIT;

		void validateStorageFlags(VertexBufferStorage storageOptions)
		{
			const GLbitfield storageFlags = toOpenGLFlags(storageOptions);
			if ((storageFlags & ~VALID_STORAGE_FLAGS) != 0)
			{
				throw VertexBufferError("VertexBuffer storage flags contain unsupported bits");
			}

			const bool allowsRead = (storageFlags & GL_MAP_READ_BIT) != 0;
			const bool allowsWrite = (storageFlags & GL_MAP_WRITE_BIT) != 0;
			const bool persistent = (storageFlags & GL_MAP_PERSISTENT_BIT) != 0;
			const bool coherent = (storageFlags & GL_MAP_COHERENT_BIT) != 0;

			if (persistent && !allowsRead && !allowsWrite)
			{
				throw VertexBufferError("Persistent VertexBuffer storage requires GL_MAP_READ_BIT or GL_MAP_WRITE_BIT");
			}

			if (coherent && !persistent)
			{
				throw VertexBufferError("Coherent VertexBuffer storage requires GL_MAP_PERSISTENT_BIT");
			}
		}

		void validateMapFlags(VertexBufferStorage storageOptions, VertexBufferMapAccess accessOptions)
		{
			const GLbitfield storageFlags = toOpenGLFlags(storageOptions);
			const GLbitfield accessFlags = toOpenGLFlags(accessOptions);
			if ((accessFlags & ~VALID_MAP_FLAGS) != 0)
			{
				throw VertexBufferError("VertexBuffer map flags contain unsupported bits");
			}

			const bool reads = (accessFlags & GL_MAP_READ_BIT) != 0;
			const bool writes = (accessFlags & GL_MAP_WRITE_BIT) != 0;
			if (!reads && !writes)
			{
				throw VertexBufferError("VertexBuffer mapping requires GL_MAP_READ_BIT or GL_MAP_WRITE_BIT");
			}

			if (reads && (accessFlags & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT)) != 0)
			{
				throw VertexBufferError("Read mappings cannot invalidate or use GL_MAP_UNSYNCHRONIZED_BIT");
			}

			if ((accessFlags & GL_MAP_FLUSH_EXPLICIT_BIT) != 0 && !writes)
			{
				throw VertexBufferError("GL_MAP_FLUSH_EXPLICIT_BIT requires GL_MAP_WRITE_BIT");
			}

			if (reads && (storageFlags & GL_MAP_READ_BIT) == 0)
			{
				throw VertexBufferError("VertexBuffer storage was not created with GL_MAP_READ_BIT");
			}

			if (writes && (storageFlags & GL_MAP_WRITE_BIT) == 0)
			{
				throw VertexBufferError("VertexBuffer storage was not created with GL_MAP_WRITE_BIT");
			}

			if ((accessFlags & GL_MAP_PERSISTENT_BIT) != 0 && (storageFlags & GL_MAP_PERSISTENT_BIT) == 0)
			{
				throw VertexBufferError("VertexBuffer storage was not created for persistent mapping");
			}

			if ((accessFlags & GL_MAP_COHERENT_BIT) != 0 && (storageFlags & GL_MAP_COHERENT_BIT) == 0)
			{
				throw VertexBufferError("VertexBuffer storage was not created for coherent mapping");
			}
		}

		[[nodiscard]] GLsizeiptr toGLSize(std::size_t value, std::string_view name)
		{
			if (value > static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max()))
			{
				throw VertexBufferError(std::string(name) + " exceeds OpenGL buffer size limits");
			}

			return static_cast<GLsizeiptr>(value);
		}

		[[nodiscard]] GLintptr toGLOffset(std::size_t value, std::string_view name)
		{
			if (value > static_cast<std::size_t>(std::numeric_limits<GLintptr>::max()))
			{
				throw VertexBufferError(std::string(name) + " exceeds OpenGL buffer offset limits");
			}

			return static_cast<GLintptr>(value);
		}
	}

	VertexBufferError::VertexBufferError(const std::string& message)
		: std::runtime_error(message)
	{
	}

	VertexBuffer::VertexBuffer(const VertexBufferDescriptor& descriptor)
		: sizeInBytes(descriptor.sizeInBytes),
		strideInBytes(descriptor.strideInBytes),
		storageOptions(descriptor.storageOptions),
		debugName(descriptor.debugName)
	{
		if (this->sizeInBytes == 0)
		{
			throw VertexBufferError("VertexBuffer size must be greater than zero");
		}

		if (this->strideInBytes == 0 || this->sizeInBytes % this->strideInBytes != 0)
		{
			throw VertexBufferError("VertexBuffer size must be an exact multiple of a non-zero vertex stride");
		}

		validateStorageFlags(this->storageOptions);
		pipeline::device::requireOpenGL46Context();
		pipeline::device::throwPendingOpenGLErrors("VertexBuffer construction precondition");

		glCreateBuffers(1, &this->ID);
		pipeline::device::throwPendingOpenGLErrors("glCreateBuffers");

		try
		{
			glNamedBufferStorage(
				this->ID,
				toGLSize(this->sizeInBytes, "VertexBuffer size"),
				descriptor.initialData,
				toOpenGLFlags(this->storageOptions));
			pipeline::device::throwPendingOpenGLErrors("glNamedBufferStorage");
			this->setDebugName(this->debugName);
		}
		catch (...)
		{
			this->release();
			throw;
		}
	}

	VertexBuffer::~VertexBuffer()
	{
		this->release();
	}

	VertexBuffer::VertexBuffer(VertexBuffer&& other) noexcept
		: ID(std::exchange(other.ID, 0)),
		sizeInBytes(std::exchange(other.sizeInBytes, 0)),
		strideInBytes(std::exchange(other.strideInBytes, 0)),
		storageOptions(std::exchange(other.storageOptions, VertexBufferStorage::None)),
		debugName(std::move(other.debugName)),
		mapped(std::exchange(other.mapped, false)),
		activeMapOptions(std::exchange(other.activeMapOptions, VertexBufferMapAccess::None)),
		activeMapOffsetInBytes(std::exchange(other.activeMapOffsetInBytes, 0)),
		activeMapSizeInBytes(std::exchange(other.activeMapSizeInBytes, 0))
	{
	}

	VertexBuffer& VertexBuffer::operator=(VertexBuffer&& other) noexcept
	{
		if (this != &other)
		{
			this->release();
			this->ID = std::exchange(other.ID, 0);
			this->sizeInBytes = std::exchange(other.sizeInBytes, 0);
			this->strideInBytes = std::exchange(other.strideInBytes, 0);
			this->storageOptions = std::exchange(other.storageOptions, VertexBufferStorage::None);
			this->debugName = std::move(other.debugName);
			this->mapped = std::exchange(other.mapped, false);
			this->activeMapOptions = std::exchange(other.activeMapOptions, VertexBufferMapAccess::None);
			this->activeMapOffsetInBytes = std::exchange(other.activeMapOffsetInBytes, 0);
			this->activeMapSizeInBytes = std::exchange(other.activeMapSizeInBytes, 0);
		}

		return *this;
	}

	void VertexBuffer::update(std::size_t destinationOffsetInBytes, const void* data, std::size_t updateSizeInBytes)
	{
		this->requireUsable();
		if (this->mapped)
		{
			throw VertexBufferError("VertexBuffer::update cannot modify a mapped buffer");
		}
		this->validateRange(destinationOffsetInBytes, updateSizeInBytes, "VertexBuffer::update");

		if (data == nullptr)
		{
			throw VertexBufferError("VertexBuffer::update requires non-null source data");
		}

		if ((toOpenGLFlags(this->storageOptions) & GL_DYNAMIC_STORAGE_BIT) == 0)
		{
			throw VertexBufferError("VertexBuffer::update requires GL_DYNAMIC_STORAGE_BIT");
		}

		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::update precondition");
		glNamedBufferSubData(this->ID, toGLOffset(destinationOffsetInBytes, "VertexBuffer update offset"), toGLSize(updateSizeInBytes, "VertexBuffer update size"), data);
		pipeline::device::throwPendingOpenGLErrors("glNamedBufferSubData");
	}

	void* VertexBuffer::mapRange(std::size_t offsetInBytes, std::size_t mapSizeInBytes, VertexBufferMapAccess accessOptions)
	{
		this->requireUsable();
		if (this->mapped)
		{
			throw VertexBufferError("VertexBuffer is already mapped");
		}

		this->validateRange(offsetInBytes, mapSizeInBytes, "VertexBuffer::mapRange");
		validateMapFlags(this->storageOptions, accessOptions);

		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::mapRange precondition");
		void* mappedMemory = glMapNamedBufferRange(this->ID, toGLOffset(offsetInBytes, "VertexBuffer map offset"), toGLSize(mapSizeInBytes, "VertexBuffer map size"), toOpenGLFlags(accessOptions));
		pipeline::device::throwPendingOpenGLErrors("glMapNamedBufferRange");

		if (mappedMemory == nullptr)
		{
			throw VertexBufferError("glMapNamedBufferRange returned null");
		}

		this->mapped = true;
		this->activeMapOptions = accessOptions;
		this->activeMapOffsetInBytes = offsetInBytes;
		this->activeMapSizeInBytes = mapSizeInBytes;
		return mappedMemory;
	}

	void VertexBuffer::flushMappedRange(std::size_t offsetInBytes, std::size_t flushSizeInBytes)
	{
		this->requireUsable();
		if (!this->mapped)
		{
			throw VertexBufferError("VertexBuffer::flushMappedRange requires an active mapping");
		}

		if ((toOpenGLFlags(this->activeMapOptions) & GL_MAP_FLUSH_EXPLICIT_BIT) == 0)
		{
			throw VertexBufferError("VertexBuffer mapping was not created with GL_MAP_FLUSH_EXPLICIT_BIT");
		}

		if (offsetInBytes < this->activeMapOffsetInBytes ||
			flushSizeInBytes > this->activeMapSizeInBytes - (offsetInBytes - this->activeMapOffsetInBytes))
		{
			throw VertexBufferError("VertexBuffer::flushMappedRange range exceeds the active mapping");
		}
		if (flushSizeInBytes == 0)
		{
			throw VertexBufferError("VertexBuffer::flushMappedRange range must not be empty");
		}
		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::flushMappedRange precondition");
		glFlushMappedNamedBufferRange(this->ID, toGLOffset(offsetInBytes, "VertexBuffer flush offset"), toGLSize(flushSizeInBytes, "VertexBuffer flush size"));
		pipeline::device::throwPendingOpenGLErrors("glFlushMappedNamedBufferRange");
	}

	void VertexBuffer::unmap()
	{
		this->requireUsable();
		if (!this->mapped)
		{
			throw VertexBufferError("VertexBuffer::unmap requires an active mapping");
		}

		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::unmap precondition");
		const GLboolean dataPreserved = glUnmapNamedBuffer(this->ID);
		this->mapped = false;
		this->activeMapOptions = VertexBufferMapAccess::None;
		this->activeMapOffsetInBytes = 0;
		this->activeMapSizeInBytes = 0;
		pipeline::device::throwPendingOpenGLErrors("glUnmapNamedBuffer");

		if (dataPreserved == GL_FALSE)
		{
			throw VertexBufferError("VertexBuffer data became corrupted while mapped; re-upload the affected data before use");
		}
	}

	void VertexBuffer::invalidateRange(std::size_t offsetInBytes, std::size_t invalidateSizeInBytes)
	{
		this->requireUsable();
		if (this->mapped)
		{
			throw VertexBufferError("VertexBuffer::invalidateRange cannot invalidate a mapped buffer");
		}
		this->validateRange(offsetInBytes, invalidateSizeInBytes, "VertexBuffer::invalidateRange");
		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::invalidateRange precondition");
		glInvalidateBufferSubData(this->ID, toGLOffset(offsetInBytes, "VertexBuffer invalidate offset"), toGLSize(invalidateSizeInBytes, "VertexBuffer invalidate size"));
		pipeline::device::throwPendingOpenGLErrors("glInvalidateBufferSubData");
	}

	void VertexBuffer::copyTo(VertexBuffer& destination, std::size_t sourceOffsetInBytes, std::size_t destinationOffsetInBytes, std::size_t copySizeInBytes) const
	{
		this->requireUsable();
		destination.requireUsable();
		if (this->mapped || destination.mapped)
		{
			throw VertexBufferError("VertexBuffer::copyTo cannot copy while either buffer is mapped");
		}
		if (this == &destination)
		{
			throw VertexBufferError("VertexBuffer::copyTo requires distinct source and destination buffers");
		}

		this->validateRange(sourceOffsetInBytes, copySizeInBytes, "VertexBuffer::copyTo source range");
		destination.validateRange(destinationOffsetInBytes, copySizeInBytes, "VertexBuffer::copyTo destination range");

		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::copyTo precondition");
		glCopyNamedBufferSubData(
			this->ID,
			destination.ID,
			toGLOffset(sourceOffsetInBytes, "VertexBuffer copy source offset"),
			toGLOffset(destinationOffsetInBytes, "VertexBuffer copy destination offset"),
			toGLSize(copySizeInBytes, "VertexBuffer copy size"));
		pipeline::device::throwPendingOpenGLErrors("glCopyNamedBufferSubData");
	}

	void VertexBuffer::bindToVertexArray(GLuint vertexArrayID, GLuint bindingIndex, std::size_t offsetInBytes) const
	{
		this->requireUsable();
		if (vertexArrayID == 0)
		{
			throw VertexBufferError("VertexBuffer::bindToVertexArray requires a valid vertex array");
		}

		if (offsetInBytes >= this->sizeInBytes)
		{
			throw VertexBufferError("VertexBuffer::bindToVertexArray offset exceeds VertexBuffer storage");
		}
		if (this->strideInBytes > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
		{
			throw VertexBufferError("VertexBuffer stride exceeds OpenGL vertex binding limits");
		}

		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::bindToVertexArray precondition");
		glVertexArrayVertexBuffer(vertexArrayID, bindingIndex, this->ID, toGLOffset(offsetInBytes, "VertexBuffer vertex-array offset"), static_cast<GLsizei>(this->strideInBytes));
		pipeline::device::throwPendingOpenGLErrors("glVertexArrayVertexBuffer");
	}

	void VertexBuffer::setDebugName(std::string_view newDebugName)
	{
		this->requireUsable();
		if (newDebugName.size() > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
		{
			throw VertexBufferError("VertexBuffer debug name exceeds OpenGL label limits");
		}

		this->debugName = newDebugName;
		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::setDebugName precondition");
		glObjectLabel(GL_BUFFER, this->ID, static_cast<GLsizei>(this->debugName.size()), this->debugName.c_str());
		pipeline::device::throwPendingOpenGLErrors("glObjectLabel");
	}

	GLuint VertexBuffer::getID() const noexcept
	{
		return this->ID;
	}

	std::size_t VertexBuffer::getSizeInBytes() const noexcept
	{
		return this->sizeInBytes;
	}

	std::size_t VertexBuffer::getStrideInBytes() const noexcept
	{
		return this->strideInBytes;
	}

	std::size_t VertexBuffer::getVertexCount() const noexcept
	{
		return this->strideInBytes == 0 ? 0 : this->sizeInBytes / this->strideInBytes;
	}

	VertexBufferStorage VertexBuffer::getStorageOptions() const noexcept
	{
		return this->storageOptions;
	}

	std::string_view VertexBuffer::getDebugName() const noexcept
	{
		return this->debugName;
	}

	bool VertexBuffer::isMapped() const noexcept
	{
		return this->mapped;
	}

	bool VertexBuffer::isStorageImmutable() const
	{
		this->requireUsable();
		GLint immutableStorage = GL_FALSE;
		pipeline::device::throwPendingOpenGLErrors("VertexBuffer::isStorageImmutable precondition");
		glGetNamedBufferParameteriv(this->ID, GL_BUFFER_IMMUTABLE_STORAGE, &immutableStorage);
		pipeline::device::throwPendingOpenGLErrors("glGetNamedBufferParameteriv");
		return immutableStorage == GL_TRUE;
	}

	void VertexBuffer::requireUsable() const
	{
		if (this->ID == 0)
		{
			throw VertexBufferError("VertexBuffer does not own an OpenGL buffer");
		}
	}

	void VertexBuffer::validateRange(std::size_t offsetInBytes, std::size_t rangeSizeInBytes, std::string_view operation) const
	{
		if (offsetInBytes > this->sizeInBytes || rangeSizeInBytes > this->sizeInBytes - offsetInBytes)
		{
			throw VertexBufferError(std::string(operation) + " range exceeds VertexBuffer storage");
		}

		if (rangeSizeInBytes == 0)
		{
			throw VertexBufferError(std::string(operation) + " range must not be empty");
		}
	}

	void VertexBuffer::release() noexcept
	{
		if (this->ID != 0)
		{
			if (this->mapped)
			{
				glUnmapNamedBuffer(this->ID);
			}

			glDeleteBuffers(1, &this->ID);
			this->ID = 0;
			this->mapped = false;
			this->activeMapOptions = VertexBufferMapAccess::None;
			this->activeMapOffsetInBytes = 0;
			this->activeMapSizeInBytes = 0;
		}
	}
}
