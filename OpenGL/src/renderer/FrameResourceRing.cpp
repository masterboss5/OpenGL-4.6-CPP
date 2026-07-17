#include "FrameResourceRing.h"

#include <stdexcept>

#include "src/pipeline/device/OpenGLRuntime.h"

namespace renderer
{
	FrameBufferSlice FrameResourceRing::createBuffer(uint64 capacityInBytes)
	{
		if (capacityInBytes == 0) throw std::invalid_argument("Frame resource capacity must be non-zero");
		FrameBufferSlice slice { .capacityInBytes = capacityInBytes };
		glCreateBuffers(1, &slice.buffer);
		const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;
		const GLbitfield mapAccessFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		glNamedBufferStorage(slice.buffer, static_cast<GLsizeiptr>(capacityInBytes), nullptr, storageFlags);
		slice.mappedMemory = glMapNamedBufferRange(slice.buffer, 0, static_cast<GLsizeiptr>(capacityInBytes), mapAccessFlags);
		if (slice.mappedMemory == nullptr)
		{
			glDeleteBuffers(1, &slice.buffer);
			throw std::runtime_error("Failed to persistently map a frame resource buffer");
		}
		return slice;
	}

	FrameResourceRing::FrameResourceRing(uint64 candidateBytes, uint64 visibleBytes, uint64 indirectBytes, uint64 batchBytes, uint64 visibilityScratchBytes)
	{
		pipeline::device::requireOpenGL46Context();
		for (Slot& slot : slots)
		{
			slot.resources.candidateInstances = createBuffer(candidateBytes);
			slot.resources.visibleInstances = createBuffer(visibleBytes);
			slot.resources.indirectCommands = createBuffer(indirectBytes);
			slot.resources.batchMetadata = createBuffer(batchBytes);
			slot.resources.visibilityScratch = createBuffer(visibilityScratchBytes);
		}
	}

	FrameResourceRing::~FrameResourceRing()
	{
		for (Slot& slot : slots)
		{
			if (slot.fence != nullptr) glDeleteSync(slot.fence);
			destroyBuffer(slot.resources.candidateInstances); destroyBuffer(slot.resources.visibleInstances); destroyBuffer(slot.resources.indirectCommands); destroyBuffer(slot.resources.batchMetadata); destroyBuffer(slot.resources.visibilityScratch);
		}
	}

	void FrameResourceRing::destroyBuffer(FrameBufferSlice& slice) noexcept
	{
		if (slice.buffer != 0)
		{
			if (slice.mappedMemory != nullptr) glUnmapNamedBuffer(slice.buffer);
			glDeleteBuffers(1, &slice.buffer);
		}
		slice = {};
	}

	void FrameResourceRing::waitForFence(GLsync& fence)
	{
		if (fence == nullptr) return;
		for (;;)
		{
			const GLenum result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000ULL);
			if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) { glDeleteSync(fence); fence = nullptr; return; }
			if (result == GL_WAIT_FAILED) throw std::runtime_error("OpenGL frame fence wait failed");
		}
	}

	FrameResources& FrameResourceRing::acquire(uint64 frameNumber)
	{
		if (activeSlot != ~uint32 { 0 }) throw std::logic_error("A frame resource slot is already acquired");
		activeSlot = static_cast<uint32>(frameNumber % FrameCount);
		Slot& slot = slots[activeSlot];
		waitForFence(slot.fence);
		slot.resources.frameNumber = frameNumber;
		return slot.resources;
	}

	void FrameResourceRing::retire()
	{
		if (activeSlot == ~uint32 { 0 }) throw std::logic_error("No frame resource slot is acquired");
		Slot& slot = slots[activeSlot];
		slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		if (slot.fence == nullptr) throw std::runtime_error("Failed to insert OpenGL frame fence");
		activeSlot = ~uint32 { 0 };
	}

	bool FrameResourceRing::isAcquired() const noexcept { return activeSlot != ~uint32 { 0 }; }
}
