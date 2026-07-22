#pragma once

#include "src/pipeline/device/Device.h"
#include "src/resource/Asset.h"
#include "src/types.h"

#include <GL/glew.h>
#include <array>
#include <vector>

namespace renderer
{
struct FrameBufferSlice final
{
	GLuint Buffer = 0;
	void *MappedMemory = nullptr;
	uint64 CapacityInBytes = 0;
};

struct FrameResources final
{
	FrameBufferSlice FrameConstants;
	FrameBufferSlice Materials;
	FrameBufferSlice ShadowData;
	FrameBufferSlice Lights;
	FrameBufferSlice CandidateInstances;
	FrameBufferSlice ShadowInstances;
	FrameBufferSlice VisibleInstances;
	FrameBufferSlice IndirectCommands;
	FrameBufferSlice BatchMetadata;
	FrameBufferSlice VisibilityScratch;
	FrameBufferSlice SkinMatrices;
	FrameBufferSlice MorphWeights;
	uint64 FrameNumber = 0;
};

struct FrameResourceCapacitySpecification final
{
	uint64 FrameConstants = 0;
	uint64 Materials = 0;
	uint64 ShadowData = 0;
	uint64 Lights = 0;
	uint64 CandidateInstances = 0;
	uint64 ShadowInstances = 0;
	uint64 VisibleInstances = 0;
	uint64 IndirectCommands = 0;
	uint64 BatchMetadata = 0;
	uint64 VisibilityScratch = 0;
	uint64 SkinMatrices = 0;
	uint64 MorphWeights = 0;
};

class FrameResourceRing final
{
  public:
	static constexpr uint32 FrameCount = 3;
	FrameResourceRing(pipeline::device::Device &Device, const FrameResourceCapacitySpecification &Capacities);
	~FrameResourceRing();
	FrameResourceRing(const FrameResourceRing &) = delete;
	FrameResourceRing &operator=(const FrameResourceRing &) = delete;
	FrameResources &Acquire(uint64 FrameNumber);
	void Retire(std::vector<resource::AssetPtr<resource::Asset>> &&AssetPins);
	void Abandon(std::vector<resource::AssetPtr<resource::Asset>> AssetPins = {}) noexcept;
	[[nodiscard]] bool IsAcquired() const noexcept;

  private:
	pipeline::device::Device *Device = nullptr;
	struct Slot final
	{
		FrameResources Resources;
		pipeline::device::DeviceSync Fence;
		std::vector<resource::AssetPtr<resource::Asset>> AssetPins;
		bool Usable = true;
	};
	std::array<Slot, FrameCount> Slots;
	uint32 ActiveSlot = ~uint32{0};
	[[nodiscard]] FrameBufferSlice CreateBuffer(uint64 CapacityInBytes);
	static void DestroyBuffer(FrameBufferSlice &Slice) noexcept;
	void DestroyAllBuffers() noexcept;
	void WaitForFence(pipeline::device::DeviceSync &Fence);
};
} // namespace renderer
