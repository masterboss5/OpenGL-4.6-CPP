#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <GL/glew.h>

namespace renderer
{
	enum class VertexBufferStorage : std::uint32_t
	{
		None = 0,
		DynamicUpdates = GL_DYNAMIC_STORAGE_BIT,
		MapRead = GL_MAP_READ_BIT,
		MapWrite = GL_MAP_WRITE_BIT,
		PersistentMapping = GL_MAP_PERSISTENT_BIT,
		CoherentMapping = GL_MAP_COHERENT_BIT,
		ClientStorage = GL_CLIENT_STORAGE_BIT
	};

	enum class VertexBufferMapAccess : std::uint32_t
	{
		None = 0,
		Read = GL_MAP_READ_BIT,
		Write = GL_MAP_WRITE_BIT,
		InvalidateRange = GL_MAP_INVALIDATE_RANGE_BIT,
		InvalidateBuffer = GL_MAP_INVALIDATE_BUFFER_BIT,
		FlushExplicit = GL_MAP_FLUSH_EXPLICIT_BIT,
		Unsynchronized = GL_MAP_UNSYNCHRONIZED_BIT,
		Persistent = GL_MAP_PERSISTENT_BIT,
		Coherent = GL_MAP_COHERENT_BIT
	};

	constexpr VertexBufferStorage operator|(VertexBufferStorage left, VertexBufferStorage right) noexcept
	{
		return static_cast<VertexBufferStorage>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}

	constexpr VertexBufferMapAccess operator|(VertexBufferMapAccess left, VertexBufferMapAccess right) noexcept
	{
		return static_cast<VertexBufferMapAccess>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}

	struct VertexBufferDescriptor final
	{
		std::size_t sizeInBytes = 0;
		std::size_t strideInBytes = 0;
		VertexBufferStorage storageOptions = VertexBufferStorage::DynamicUpdates;
		const void* initialData = nullptr;
		std::string_view debugName = "VertexBuffer";
	};

	class VertexBufferError final : public std::runtime_error
	{
	public:
		explicit VertexBufferError(const std::string& message);
	};

	class VertexBuffer final
	{
	public:
		explicit VertexBuffer(const VertexBufferDescriptor& descriptor);
		~VertexBuffer();

		VertexBuffer(const VertexBuffer&) = delete;
		VertexBuffer& operator=(const VertexBuffer&) = delete;

		VertexBuffer(VertexBuffer&& other) noexcept;
		VertexBuffer& operator=(VertexBuffer&& other) noexcept;

		template<typename TVertex>
		requires std::is_trivially_copyable_v<TVertex>
		[[nodiscard]] static VertexBuffer create(
			std::span<const TVertex> vertices,
			VertexBufferStorage storageOptions = VertexBufferStorage::DynamicUpdates,
			std::string_view debugName = "VertexBuffer")
		{
			if (vertices.empty())
			{
				throw VertexBufferError("VertexBuffer::create requires at least one vertex");
			}

			if (vertices.size() > std::numeric_limits<std::size_t>::max() / sizeof(TVertex))
			{
				throw VertexBufferError("VertexBuffer::create vertex data exceeds addressable storage");
			}

			return VertexBuffer({
				.sizeInBytes = sizeof(TVertex) * vertices.size(),
				.strideInBytes = sizeof(TVertex),
				.storageOptions = storageOptions,
				.initialData = vertices.data(),
				.debugName = debugName
			});
		}

		void update(std::size_t destinationOffsetInBytes, const void* data, std::size_t sizeInBytes);

		template<typename TVertex>
		requires std::is_trivially_copyable_v<TVertex>
		void updateVertices(std::size_t firstVertex, std::span<const TVertex> vertices)
		{
			this->requireUsable();
			if (sizeof(TVertex) != this->strideInBytes)
			{
				throw VertexBufferError("VertexBuffer::updateVertices vertex type does not match the buffer stride");
			}

			if (firstVertex > std::numeric_limits<std::size_t>::max() / this->strideInBytes ||
				vertices.size() > std::numeric_limits<std::size_t>::max() / sizeof(TVertex))
			{
				throw VertexBufferError("VertexBuffer::updateVertices range exceeds addressable storage");
			}

			this->update(firstVertex * this->strideInBytes, vertices.data(), sizeof(TVertex) * vertices.size());
		}

		[[nodiscard]] void* mapRange(std::size_t offsetInBytes, std::size_t sizeInBytes, VertexBufferMapAccess accessOptions);
		void flushMappedRange(std::size_t offsetInBytes, std::size_t sizeInBytes);
		void unmap();
		void invalidateRange(std::size_t offsetInBytes, std::size_t sizeInBytes);
		void copyTo(VertexBuffer& destination, std::size_t sourceOffsetInBytes, std::size_t destinationOffsetInBytes, std::size_t sizeInBytes) const;

		void bindToVertexArray(GLuint vertexArrayID, GLuint bindingIndex, std::size_t offsetInBytes = 0) const;
		void setDebugName(std::string_view debugName);

		[[nodiscard]] GLuint getID() const noexcept;
		[[nodiscard]] std::size_t getSizeInBytes() const noexcept;
		[[nodiscard]] std::size_t getStrideInBytes() const noexcept;
		[[nodiscard]] std::size_t getVertexCount() const noexcept;
		[[nodiscard]] VertexBufferStorage getStorageOptions() const noexcept;
		[[nodiscard]] std::string_view getDebugName() const noexcept;
		[[nodiscard]] bool isMapped() const noexcept;
		[[nodiscard]] bool isStorageImmutable() const;

	private:
		GLuint ID = 0;
		std::size_t sizeInBytes = 0;
		std::size_t strideInBytes = 0;
		VertexBufferStorage storageOptions = VertexBufferStorage::None;
		std::string debugName;
		bool mapped = false;
		VertexBufferMapAccess activeMapOptions = VertexBufferMapAccess::None;
		std::size_t activeMapOffsetInBytes = 0;
		std::size_t activeMapSizeInBytes = 0;

		void requireUsable() const;
		void validateRange(std::size_t offsetInBytes, std::size_t rangeSizeInBytes, std::string_view operation) const;
		void release() noexcept;
	};
}
