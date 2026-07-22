#pragma once

#include "src/concepts.h"
#include "src/types.h"

#include <GL/glew.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace pipeline::device
{
class Device;
}

namespace renderer
{
enum class VertexBufferStorage : uint32
{
	None = 0,
	DynamicUpdates = GL_DYNAMIC_STORAGE_BIT,
	MapRead = GL_MAP_READ_BIT,
	MapWrite = GL_MAP_WRITE_BIT,
	PersistentMapping = GL_MAP_PERSISTENT_BIT,
	CoherentMapping = GL_MAP_COHERENT_BIT,
	ClientStorage = GL_CLIENT_STORAGE_BIT
};

enum class VertexBufferMapAccess : uint32
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

constexpr VertexBufferStorage operator|(VertexBufferStorage Left, VertexBufferStorage Right) noexcept
{
	return static_cast<VertexBufferStorage>(static_cast<uint32>(Left) | static_cast<uint32>(Right));
}

constexpr VertexBufferMapAccess operator|(VertexBufferMapAccess Left, VertexBufferMapAccess Right) noexcept
{
	return static_cast<VertexBufferMapAccess>(static_cast<uint32>(Left) | static_cast<uint32>(Right));
}

struct VertexBufferDescriptor final
{
	usize SizeInBytes = 0;
	usize StrideInBytes = 0;
	VertexBufferStorage StorageOptions = VertexBufferStorage::DynamicUpdates;
	const void *InitialData = nullptr;
	std::string_view DebugName = "VertexBuffer";
};

class VertexBufferError final : public std::runtime_error
{
  public:
	explicit VertexBufferError(const std::string &Message);
};

class VertexBuffer final
{
  public:
	VertexBuffer(pipeline::device::Device &Device, const VertexBufferDescriptor &Descriptor);
	~VertexBuffer();

	VertexBuffer(const VertexBuffer &) = delete;
	VertexBuffer &operator=(const VertexBuffer &) = delete;

	VertexBuffer(VertexBuffer &&Other) noexcept;
	VertexBuffer &operator=(VertexBuffer &&Other) noexcept;

	template <TriviallyCopyable TVertex>
	[[nodiscard]] static VertexBuffer Create(pipeline::device::Device &Device, std::span<const TVertex> Vertices,
											 VertexBufferStorage StorageOptions = VertexBufferStorage::DynamicUpdates,
											 std::string_view DebugName = "VertexBuffer")
	{
		if (Vertices.empty())
		{
			throw VertexBufferError("VertexBuffer::create requires at least one vertex");
		}

		if (Vertices.size() > std::numeric_limits<usize>::max() / sizeof(TVertex))
		{
			throw VertexBufferError("VertexBuffer::create vertex data exceeds addressable storage");
		}

		return VertexBuffer(Device, {.SizeInBytes = sizeof(TVertex) * Vertices.size(),
									 .StrideInBytes = sizeof(TVertex),
									 .StorageOptions = StorageOptions,
									 .InitialData = Vertices.data(),
									 .DebugName = DebugName});
	}

	void Update(usize DestinationOffsetInBytes, const void *Data, usize SizeInBytes);

	template <TriviallyCopyable TVertex> void UpdateVertices(usize FirstVertex, std::span<const TVertex> Vertices)
	{
		this->RequireUsable();
		if (sizeof(TVertex) != this->StrideInBytes)
		{
			throw VertexBufferError("VertexBuffer::updateVertices vertex type does not match the buffer stride");
		}

		if (FirstVertex > std::numeric_limits<usize>::max() / this->StrideInBytes ||
			Vertices.size() > std::numeric_limits<usize>::max() / sizeof(TVertex))
		{
			throw VertexBufferError("VertexBuffer::updateVertices range exceeds addressable storage");
		}

		this->Update(FirstVertex * this->StrideInBytes, Vertices.data(), sizeof(TVertex) * Vertices.size());
	}

	[[nodiscard]] void *MapRange(usize OffsetInBytes, usize SizeInBytes, VertexBufferMapAccess AccessOptions);
	void FlushMappedRange(usize OffsetInBytes, usize SizeInBytes);
	void Unmap();
	void InvalidateRange(usize OffsetInBytes, usize SizeInBytes);
	void CopyTo(VertexBuffer &Destination, usize SourceOffsetInBytes, usize DestinationOffsetInBytes, usize SizeInBytes) const;

	void BindToVertexArray(GLuint VertexArrayID, GLuint BindingIndex, usize OffsetInBytes = 0) const;
	void SetDebugName(std::string_view DebugName);

	[[nodiscard]] GLuint GetID() const noexcept;
	[[nodiscard]] usize GetSizeInBytes() const noexcept;
	[[nodiscard]] usize GetStrideInBytes() const noexcept;
	[[nodiscard]] usize GetVertexCount() const noexcept;
	[[nodiscard]] VertexBufferStorage GetStorageOptions() const noexcept;
	[[nodiscard]] std::string_view GetDebugName() const noexcept;
	[[nodiscard]] bool IsMapped() const noexcept;
	[[nodiscard]] bool IsStorageImmutable() const;
	[[nodiscard]] pipeline::device::Device &GetDevice() const;

  private:
	pipeline::device::Device *Device = nullptr;
	GLuint ID = 0;
	usize SizeInBytes = 0;
	usize StrideInBytes = 0;
	VertexBufferStorage StorageOptions = VertexBufferStorage::None;
	std::string DebugName;
	bool Mapped = false;
	VertexBufferMapAccess ActiveMapOptions = VertexBufferMapAccess::None;
	usize ActiveMapOffsetInBytes = 0;
	usize ActiveMapSizeInBytes = 0;

	void RequireUsable() const;
	void ValidateRange(usize OffsetInBytes, usize RangeSizeInBytes, std::string_view Operation) const;
	void Release() noexcept;
};
} // namespace renderer
