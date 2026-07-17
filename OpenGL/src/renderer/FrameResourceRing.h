#pragma once

#include <array>

#include <GL/glew.h>

#include "src/types.h"

namespace renderer
{
	struct FrameBufferSlice final
	{
		GLuint buffer = 0;
		void* mappedMemory = nullptr;
		uint64 capacityInBytes = 0;
	};

	struct FrameResources final
	{
		FrameBufferSlice candidateInstances;
		FrameBufferSlice visibleInstances;
		FrameBufferSlice indirectCommands;
		FrameBufferSlice batchMetadata;
		FrameBufferSlice visibilityScratch;
		uint64 frameNumber = 0;
	};

	class FrameResourceRing final
	{
	public:
		static constexpr uint32 FrameCount = 3;
		FrameResourceRing(uint64 candidateBytes, uint64 visibleBytes, uint64 indirectBytes, uint64 batchBytes, uint64 visibilityScratchBytes);
		~FrameResourceRing();
		FrameResourceRing(const FrameResourceRing&) = delete;
		FrameResourceRing& operator=(const FrameResourceRing&) = delete;
		FrameResources& acquire(uint64 frameNumber);
		void retire();
		[[nodiscard]] bool isAcquired() const noexcept;
	private:
		struct Slot final { FrameResources resources; GLsync fence = nullptr; };
		std::array<Slot, FrameCount> slots;
		uint32 activeSlot = ~uint32 { 0 };
		[[nodiscard]] static FrameBufferSlice createBuffer(uint64 capacityInBytes);
		static void destroyBuffer(FrameBufferSlice& slice) noexcept;
		static void waitForFence(GLsync& fence);
	};
}
