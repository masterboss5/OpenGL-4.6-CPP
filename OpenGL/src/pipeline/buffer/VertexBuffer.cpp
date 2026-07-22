#include "VertexBuffer.h"

#include "src/pipeline/device/Device.h"

#include <limits>
#include <utility>

namespace renderer
{
namespace
{
[[nodiscard]] constexpr GLbitfield ToOpenGLFlags(VertexBufferStorage Options)
{
	return static_cast<GLbitfield>(Options);
}

[[nodiscard]] constexpr GLbitfield ToOpenGLFlags(VertexBufferMapAccess Options)
{
	return static_cast<GLbitfield>(Options);
}

constexpr GLbitfield ValidStorageFlags =
	GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT;

constexpr GLbitfield ValidMapFlags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
									 GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

void ValidateStorageFlags(VertexBufferStorage StorageOptions)
{
	const GLbitfield StorageFlags = ToOpenGLFlags(StorageOptions);
	if ((StorageFlags & ~ValidStorageFlags) != 0)
	{
		throw VertexBufferError("VertexBuffer storage flags contain unsupported bits");
	}

	const bool AllowsRead = (StorageFlags & GL_MAP_READ_BIT) != 0;
	const bool AllowsWrite = (StorageFlags & GL_MAP_WRITE_BIT) != 0;
	const bool Persistent = (StorageFlags & GL_MAP_PERSISTENT_BIT) != 0;
	const bool Coherent = (StorageFlags & GL_MAP_COHERENT_BIT) != 0;

	if (Persistent && !AllowsRead && !AllowsWrite)
	{
		throw VertexBufferError("Persistent VertexBuffer storage requires GL_MAP_READ_BIT or GL_MAP_WRITE_BIT");
	}

	if (Coherent && !Persistent)
	{
		throw VertexBufferError("Coherent VertexBuffer storage requires GL_MAP_PERSISTENT_BIT");
	}
}

void ValidateMapFlags(VertexBufferStorage StorageOptions, VertexBufferMapAccess AccessOptions)
{
	const GLbitfield StorageFlags = ToOpenGLFlags(StorageOptions);
	const GLbitfield AccessFlags = ToOpenGLFlags(AccessOptions);
	if ((AccessFlags & ~ValidMapFlags) != 0)
	{
		throw VertexBufferError("VertexBuffer map flags contain unsupported bits");
	}

	const bool Reads = (AccessFlags & GL_MAP_READ_BIT) != 0;
	const bool Writes = (AccessFlags & GL_MAP_WRITE_BIT) != 0;
	if (!Reads && !Writes)
	{
		throw VertexBufferError("VertexBuffer mapping requires GL_MAP_READ_BIT or GL_MAP_WRITE_BIT");
	}

	if (Reads && (AccessFlags & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT)) != 0)
	{
		throw VertexBufferError("Read mappings cannot invalidate or use GL_MAP_UNSYNCHRONIZED_BIT");
	}

	if ((AccessFlags & GL_MAP_FLUSH_EXPLICIT_BIT) != 0 && !Writes)
	{
		throw VertexBufferError("GL_MAP_FLUSH_EXPLICIT_BIT requires GL_MAP_WRITE_BIT");
	}

	if (Reads && (StorageFlags & GL_MAP_READ_BIT) == 0)
	{
		throw VertexBufferError("VertexBuffer storage was not created with GL_MAP_READ_BIT");
	}

	if (Writes && (StorageFlags & GL_MAP_WRITE_BIT) == 0)
	{
		throw VertexBufferError("VertexBuffer storage was not created with GL_MAP_WRITE_BIT");
	}

	if ((AccessFlags & GL_MAP_PERSISTENT_BIT) != 0 && (StorageFlags & GL_MAP_PERSISTENT_BIT) == 0)
	{
		throw VertexBufferError("VertexBuffer storage was not created for persistent mapping");
	}

	if ((AccessFlags & GL_MAP_COHERENT_BIT) != 0 && (StorageFlags & GL_MAP_COHERENT_BIT) == 0)
	{
		throw VertexBufferError("VertexBuffer storage was not created for coherent mapping");
	}
}

[[nodiscard]] GLsizeiptr ToGLSize(usize Value, std::string_view Name)
{
	if (Value > static_cast<usize>(std::numeric_limits<GLsizeiptr>::max()))
	{
		throw VertexBufferError(std::string(Name) + " exceeds OpenGL buffer size limits");
	}

	return static_cast<GLsizeiptr>(Value);
}

[[nodiscard]] GLintptr ToGLOffset(usize Value, std::string_view Name)
{
	if (Value > static_cast<usize>(std::numeric_limits<GLintptr>::max()))
	{
		throw VertexBufferError(std::string(Name) + " exceeds OpenGL buffer offset limits");
	}

	return static_cast<GLintptr>(Value);
}
} // namespace

VertexBufferError::VertexBufferError(const std::string &Message) : std::runtime_error(Message)
{
}

VertexBuffer::VertexBuffer(pipeline::device::Device &Device, const VertexBufferDescriptor &Descriptor)
	: Device(&Device), SizeInBytes(Descriptor.SizeInBytes), StrideInBytes(Descriptor.StrideInBytes),
	  StorageOptions(Descriptor.StorageOptions), DebugName(Descriptor.DebugName)
{
	if (this->SizeInBytes == 0)
	{
		throw VertexBufferError("VertexBuffer size must be greater than zero");
	}

	if (this->StrideInBytes == 0 || this->SizeInBytes % this->StrideInBytes != 0)
	{
		throw VertexBufferError("VertexBuffer size must be an exact multiple of a non-zero vertex stride");
	}

	ValidateStorageFlags(this->StorageOptions);
	(void)this->Device->RequireCurrentContext();
	this->Device->CheckErrors("VertexBuffer construction precondition");

	glCreateBuffers(1, &this->ID);
	this->Device->CheckErrors("glCreateBuffers");

	try
	{
		glNamedBufferStorage(this->ID, ToGLSize(this->SizeInBytes, "VertexBuffer size"), Descriptor.InitialData,
							 ToOpenGLFlags(this->StorageOptions));
		this->Device->CheckErrors("glNamedBufferStorage");
		this->SetDebugName(this->DebugName);
	}
	catch (...)
	{
		this->Release();
		throw;
	}
}

VertexBuffer::~VertexBuffer()
{
	this->Release();
}

VertexBuffer::VertexBuffer(VertexBuffer &&Other) noexcept
	: Device(std::exchange(Other.Device, nullptr)), ID(std::exchange(Other.ID, 0)), SizeInBytes(std::exchange(Other.SizeInBytes, 0)),
	  StrideInBytes(std::exchange(Other.StrideInBytes, 0)), StorageOptions(std::exchange(Other.StorageOptions, VertexBufferStorage::None)),
	  DebugName(std::move(Other.DebugName)), Mapped(std::exchange(Other.Mapped, false)),
	  ActiveMapOptions(std::exchange(Other.ActiveMapOptions, VertexBufferMapAccess::None)),
	  ActiveMapOffsetInBytes(std::exchange(Other.ActiveMapOffsetInBytes, 0)),
	  ActiveMapSizeInBytes(std::exchange(Other.ActiveMapSizeInBytes, 0))
{
}

VertexBuffer &VertexBuffer::operator=(VertexBuffer &&Other) noexcept
{
	if (this != &Other)
	{
		this->Release();
		this->Device = std::exchange(Other.Device, nullptr);
		this->ID = std::exchange(Other.ID, 0);
		this->SizeInBytes = std::exchange(Other.SizeInBytes, 0);
		this->StrideInBytes = std::exchange(Other.StrideInBytes, 0);
		this->StorageOptions = std::exchange(Other.StorageOptions, VertexBufferStorage::None);
		this->DebugName = std::move(Other.DebugName);
		this->Mapped = std::exchange(Other.Mapped, false);
		this->ActiveMapOptions = std::exchange(Other.ActiveMapOptions, VertexBufferMapAccess::None);
		this->ActiveMapOffsetInBytes = std::exchange(Other.ActiveMapOffsetInBytes, 0);
		this->ActiveMapSizeInBytes = std::exchange(Other.ActiveMapSizeInBytes, 0);
	}

	return *this;
}

void VertexBuffer::Update(usize DestinationOffsetInBytes, const void *Data, usize UpdateSizeInBytes)
{
	this->RequireUsable();
	if (this->Mapped)
	{
		throw VertexBufferError("VertexBuffer::update cannot modify a mapped buffer");
	}
	this->ValidateRange(DestinationOffsetInBytes, UpdateSizeInBytes, "VertexBuffer::update");

	if (Data == nullptr)
	{
		throw VertexBufferError("VertexBuffer::update requires non-null source data");
	}

	if ((ToOpenGLFlags(this->StorageOptions) & GL_DYNAMIC_STORAGE_BIT) == 0)
	{
		throw VertexBufferError("VertexBuffer::update requires GL_DYNAMIC_STORAGE_BIT");
	}

	this->Device->CheckErrors("VertexBuffer::update precondition");
	glNamedBufferSubData(this->ID, ToGLOffset(DestinationOffsetInBytes, "VertexBuffer update offset"),
						 ToGLSize(UpdateSizeInBytes, "VertexBuffer update size"), Data);
	this->Device->CheckErrors("glNamedBufferSubData");
}

void *VertexBuffer::MapRange(usize OffsetInBytes, usize MapSizeInBytes, VertexBufferMapAccess AccessOptions)
{
	this->RequireUsable();
	if (this->Mapped)
	{
		throw VertexBufferError("VertexBuffer is already mapped");
	}

	this->ValidateRange(OffsetInBytes, MapSizeInBytes, "VertexBuffer::mapRange");
	ValidateMapFlags(this->StorageOptions, AccessOptions);

	this->Device->CheckErrors("VertexBuffer::mapRange precondition");
	void *MappedMemory = glMapNamedBufferRange(this->ID, ToGLOffset(OffsetInBytes, "VertexBuffer map offset"),
											   ToGLSize(MapSizeInBytes, "VertexBuffer map size"), ToOpenGLFlags(AccessOptions));
	this->Device->CheckErrors("glMapNamedBufferRange");

	if (MappedMemory == nullptr)
	{
		throw VertexBufferError("glMapNamedBufferRange returned null");
	}

	this->Mapped = true;
	this->ActiveMapOptions = AccessOptions;
	this->ActiveMapOffsetInBytes = OffsetInBytes;
	this->ActiveMapSizeInBytes = MapSizeInBytes;
	return MappedMemory;
}

void VertexBuffer::FlushMappedRange(usize OffsetInBytes, usize FlushSizeInBytes)
{
	this->RequireUsable();
	if (!this->Mapped)
	{
		throw VertexBufferError("VertexBuffer::FlushMappedRange requires an active mapping");
	}

	if ((ToOpenGLFlags(this->ActiveMapOptions) & GL_MAP_FLUSH_EXPLICIT_BIT) == 0)
	{
		throw VertexBufferError("VertexBuffer mapping was not created with GL_MAP_FLUSH_EXPLICIT_BIT");
	}

	if (OffsetInBytes < this->ActiveMapOffsetInBytes ||
		FlushSizeInBytes > this->ActiveMapSizeInBytes - (OffsetInBytes - this->ActiveMapOffsetInBytes))
	{
		throw VertexBufferError("VertexBuffer::FlushMappedRange range exceeds the active mapping");
	}
	if (FlushSizeInBytes == 0)
	{
		throw VertexBufferError("VertexBuffer::FlushMappedRange range must not be empty");
	}
	this->Device->CheckErrors("VertexBuffer::FlushMappedRange precondition");
	glFlushMappedNamedBufferRange(this->ID, ToGLOffset(OffsetInBytes, "VertexBuffer flush offset"),
								  ToGLSize(FlushSizeInBytes, "VertexBuffer flush size"));
	this->Device->CheckErrors("glFlushMappedNamedBufferRange");
}

void VertexBuffer::Unmap()
{
	this->RequireUsable();
	if (!this->Mapped)
	{
		throw VertexBufferError("VertexBuffer::unmap requires an active mapping");
	}

	this->Device->CheckErrors("VertexBuffer::unmap precondition");
	const GLboolean DataPreserved = glUnmapNamedBuffer(this->ID);
	this->Mapped = false;
	this->ActiveMapOptions = VertexBufferMapAccess::None;
	this->ActiveMapOffsetInBytes = 0;
	this->ActiveMapSizeInBytes = 0;
	this->Device->CheckErrors("glUnmapNamedBuffer");

	if (DataPreserved == GL_FALSE)
	{
		throw VertexBufferError("VertexBuffer data became corrupted while mapped; re-upload the affected data before use");
	}
}

void VertexBuffer::InvalidateRange(usize OffsetInBytes, usize InvalidateSizeInBytes)
{
	this->RequireUsable();
	if (this->Mapped)
	{
		throw VertexBufferError("VertexBuffer::InvalidateRange cannot invalidate a mapped buffer");
	}
	this->ValidateRange(OffsetInBytes, InvalidateSizeInBytes, "VertexBuffer::InvalidateRange");
	this->Device->CheckErrors("VertexBuffer::InvalidateRange precondition");
	glInvalidateBufferSubData(this->ID, ToGLOffset(OffsetInBytes, "VertexBuffer invalidate offset"),
							  ToGLSize(InvalidateSizeInBytes, "VertexBuffer invalidate size"));
	this->Device->CheckErrors("glInvalidateBufferSubData");
}

void VertexBuffer::CopyTo(VertexBuffer &Destination, usize SourceOffsetInBytes, usize DestinationOffsetInBytes, usize CopySizeInBytes) const
{
	this->RequireUsable();
	Destination.RequireUsable();
	if (this->Device != Destination.Device)
		throw VertexBufferError("VertexBuffer::copyTo requires buffers owned by the same Device");
	if (this->Mapped || Destination.Mapped)
	{
		throw VertexBufferError("VertexBuffer::copyTo cannot copy while either buffer is mapped");
	}
	if (this == &Destination)
	{
		throw VertexBufferError("VertexBuffer::copyTo requires distinct source and destination buffers");
	}

	this->ValidateRange(SourceOffsetInBytes, CopySizeInBytes, "VertexBuffer::copyTo source range");
	Destination.ValidateRange(DestinationOffsetInBytes, CopySizeInBytes, "VertexBuffer::copyTo destination range");

	this->Device->CheckErrors("VertexBuffer::copyTo precondition");
	glCopyNamedBufferSubData(this->ID, Destination.ID, ToGLOffset(SourceOffsetInBytes, "VertexBuffer copy source offset"),
							 ToGLOffset(DestinationOffsetInBytes, "VertexBuffer copy destination offset"),
							 ToGLSize(CopySizeInBytes, "VertexBuffer copy size"));
	this->Device->CheckErrors("glCopyNamedBufferSubData");
}

void VertexBuffer::BindToVertexArray(GLuint VertexArrayID, GLuint BindingIndex, usize OffsetInBytes) const
{
	this->RequireUsable();
	if (VertexArrayID == 0)
	{
		throw VertexBufferError("VertexBuffer::BindToVertexArray requires a valid vertex array");
	}

	if (OffsetInBytes >= this->SizeInBytes)
	{
		throw VertexBufferError("VertexBuffer::BindToVertexArray offset exceeds VertexBuffer storage");
	}
	if (this->StrideInBytes > static_cast<usize>(std::numeric_limits<GLsizei>::max()))
	{
		throw VertexBufferError("VertexBuffer stride exceeds OpenGL vertex binding limits");
	}

	this->Device->CheckErrors("VertexBuffer::BindToVertexArray precondition");
	glVertexArrayVertexBuffer(VertexArrayID, BindingIndex, this->ID, ToGLOffset(OffsetInBytes, "VertexBuffer vertex-array offset"),
							  static_cast<GLsizei>(this->StrideInBytes));
	this->Device->CheckErrors("glVertexArrayVertexBuffer");
}

void VertexBuffer::SetDebugName(std::string_view NewDebugName)
{
	this->RequireUsable();
	if (NewDebugName.size() > static_cast<usize>(std::numeric_limits<GLsizei>::max()))
	{
		throw VertexBufferError("VertexBuffer debug name exceeds OpenGL label limits");
	}

	this->DebugName = NewDebugName;
	this->Device->CheckErrors("VertexBuffer::setDebugName precondition");
	glObjectLabel(GL_BUFFER, this->ID, static_cast<GLsizei>(this->DebugName.size()), this->DebugName.c_str());
	this->Device->CheckErrors("glObjectLabel");
}

GLuint VertexBuffer::GetID() const noexcept
{
	return this->ID;
}

usize VertexBuffer::GetSizeInBytes() const noexcept
{
	return this->SizeInBytes;
}

usize VertexBuffer::GetStrideInBytes() const noexcept
{
	return this->StrideInBytes;
}

usize VertexBuffer::GetVertexCount() const noexcept
{
	return this->StrideInBytes == 0 ? 0 : this->SizeInBytes / this->StrideInBytes;
}

VertexBufferStorage VertexBuffer::GetStorageOptions() const noexcept
{
	return this->StorageOptions;
}

std::string_view VertexBuffer::GetDebugName() const noexcept
{
	return this->DebugName;
}

bool VertexBuffer::IsMapped() const noexcept
{
	return this->Mapped;
}

bool VertexBuffer::IsStorageImmutable() const
{
	this->RequireUsable();
	GLint ImmutableStorage = GL_FALSE;
	this->Device->CheckErrors("VertexBuffer::isStorageImmutable precondition");
	glGetNamedBufferParameteriv(this->ID, GL_BUFFER_IMMUTABLE_STORAGE, &ImmutableStorage);
	this->Device->CheckErrors("glGetNamedBufferParameteriv");
	return ImmutableStorage == GL_TRUE;
}

pipeline::device::Device &VertexBuffer::GetDevice() const
{
	this->RequireUsable();
	return *this->Device;
}

void VertexBuffer::RequireUsable() const
{
	if (this->Device == nullptr)
		throw VertexBufferError("VertexBuffer has no owning Device");
	(void)this->Device->RequireCurrentContext();
	if (this->ID == 0)
	{
		throw VertexBufferError("VertexBuffer does not own an OpenGL buffer");
	}
}

void VertexBuffer::ValidateRange(usize OffsetInBytes, usize RangeSizeInBytes, std::string_view Operation) const
{
	if (OffsetInBytes > this->SizeInBytes || RangeSizeInBytes > this->SizeInBytes - OffsetInBytes)
	{
		throw VertexBufferError(std::string(Operation) + " range exceeds VertexBuffer storage");
	}

	if (RangeSizeInBytes == 0)
	{
		throw VertexBufferError(std::string(Operation) + " range must not be empty");
	}
}

void VertexBuffer::Release() noexcept
{
	if (this->ID != 0)
	{
		const bool CanReleaseGPUResource = this->Device != nullptr && this->Device->CanIssueCommands();
		if (this->Mapped && CanReleaseGPUResource)
		{
			glUnmapNamedBuffer(this->ID);
		}

		if (CanReleaseGPUResource)
			glDeleteBuffers(1, &this->ID);
		this->ID = 0;
		this->Mapped = false;
		this->ActiveMapOptions = VertexBufferMapAccess::None;
		this->ActiveMapOffsetInBytes = 0;
		this->ActiveMapSizeInBytes = 0;
	}
}
} // namespace renderer
