#include "FrameResourceRing.h"

#include "src/pipeline/device/Device.h"

#include <stdexcept>

namespace renderer
{
FrameBufferSlice FrameResourceRing::CreateBuffer(uint64 CapacityInBytes)
{
	if (CapacityInBytes == 0)
		throw std::invalid_argument("Frame resource capacity must be non-zero");
	FrameBufferSlice Slice{.CapacityInBytes = CapacityInBytes};
	glCreateBuffers(1, &Slice.Buffer);
	const GLbitfield StorageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;
	const GLbitfield MapAccessFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
	glNamedBufferStorage(Slice.Buffer, static_cast<GLsizeiptr>(CapacityInBytes), nullptr, StorageFlags);
	Slice.MappedMemory = glMapNamedBufferRange(Slice.Buffer, 0, static_cast<GLsizeiptr>(CapacityInBytes), MapAccessFlags);
	if (Slice.MappedMemory == nullptr)
	{
		glDeleteBuffers(1, &Slice.Buffer);
		throw std::runtime_error("Failed to persistently map a frame resource buffer");
	}
	return Slice;
}

FrameResourceRing::FrameResourceRing(pipeline::device::Device &Device, const FrameResourceCapacitySpecification &Capacities)
	: Device(&Device)
{
	(void)this->Device->RequireCurrentContext();
	try
	{
		for (Slot &Slot : Slots)
		{
			Slot.Resources.FrameConstants = CreateBuffer(Capacities.FrameConstants);
			Slot.Resources.Materials = CreateBuffer(Capacities.Materials);
			Slot.Resources.ShadowData = CreateBuffer(Capacities.ShadowData);
			Slot.Resources.Lights = CreateBuffer(Capacities.Lights);
			Slot.Resources.CandidateInstances = CreateBuffer(Capacities.CandidateInstances);
			Slot.Resources.ShadowInstances = CreateBuffer(Capacities.ShadowInstances);
			Slot.Resources.VisibleInstances = CreateBuffer(Capacities.VisibleInstances);
			Slot.Resources.IndirectCommands = CreateBuffer(Capacities.IndirectCommands);
			Slot.Resources.BatchMetadata = CreateBuffer(Capacities.BatchMetadata);
			Slot.Resources.VisibilityScratch = CreateBuffer(Capacities.VisibilityScratch);
			Slot.Resources.SkinMatrices = CreateBuffer(Capacities.SkinMatrices);
			Slot.Resources.MorphWeights = CreateBuffer(Capacities.MorphWeights);
		}
		this->Device->CheckErrors("FrameResourceRing creation");
	}
	catch (...)
	{
		this->DestroyAllBuffers();
		throw;
	}
}

FrameResourceRing::~FrameResourceRing()
{
	const bool CanReleaseGPUResources = this->Device != nullptr && this->Device->CanIssueCommands();
	for (Slot &Slot : Slots)
	{
		Slot.AssetPins.clear();
		if (!CanReleaseGPUResources)
			continue;
		if (Slot.Fence.IsValid())
		{
			try
			{
				this->Device->DestroySync(Slot.Fence);
			}
			catch (...)
			{
				Slot.Fence = {};
			}
		}
	}
	if (CanReleaseGPUResources)
		this->DestroyAllBuffers();
}

void FrameResourceRing::DestroyBuffer(FrameBufferSlice &Slice) noexcept
{
	if (Slice.Buffer != 0)
	{
		if (Slice.MappedMemory != nullptr)
			glUnmapNamedBuffer(Slice.Buffer);
		glDeleteBuffers(1, &Slice.Buffer);
	}
	Slice = {};
}

void FrameResourceRing::DestroyAllBuffers() noexcept
{
	for (Slot &Slot : this->Slots)
	{
		DestroyBuffer(Slot.Resources.FrameConstants);
		DestroyBuffer(Slot.Resources.Materials);
		DestroyBuffer(Slot.Resources.ShadowData);
		DestroyBuffer(Slot.Resources.Lights);
		DestroyBuffer(Slot.Resources.CandidateInstances);
		DestroyBuffer(Slot.Resources.ShadowInstances);
		DestroyBuffer(Slot.Resources.VisibleInstances);
		DestroyBuffer(Slot.Resources.IndirectCommands);
		DestroyBuffer(Slot.Resources.BatchMetadata);
		DestroyBuffer(Slot.Resources.VisibilityScratch);
		DestroyBuffer(Slot.Resources.SkinMatrices);
		DestroyBuffer(Slot.Resources.MorphWeights);
	}
}

void FrameResourceRing::WaitForFence(pipeline::device::DeviceSync &Fence)
{
	if (!Fence.IsValid())
		return;
	for (;;)
	{
		if (this->Device->WaitSync(Fence, 1'000'000ULL) == pipeline::device::SyncWaitResult::Signaled)
		{
			this->Device->DestroySync(Fence);
			return;
		}
	}
}

FrameResources &FrameResourceRing::Acquire(uint64 FrameNumber)
{
	(void)this->Device->RequireCurrentContext();
	if (ActiveSlot != ~uint32{0})
		throw std::logic_error("A frame resource slot is already acquired");
	ActiveSlot = static_cast<uint32>(FrameNumber % FrameCount);
	Slot &Slot = Slots[ActiveSlot];
	if (!Slot.Usable)
	{
		ActiveSlot = ~uint32{0};
		throw std::runtime_error("A frame resource slot was abandoned after GPU synchronization failed");
	}
	WaitForFence(Slot.Fence);
	Slot.AssetPins.clear();
	Slot.Resources.FrameNumber = FrameNumber;
	return Slot.Resources;
}

void FrameResourceRing::Retire(std::vector<resource::AssetPtr<resource::Asset>> &&AssetPins)
{
	(void)this->Device->RequireCurrentContext();
	if (ActiveSlot == ~uint32{0})
		throw std::logic_error("No frame resource slot is acquired");
	Slot &Slot = Slots[ActiveSlot];
	pipeline::device::DeviceSync Fence = this->Device->CreateSync();
	Slot.AssetPins = std::move(AssetPins);
	Slot.Fence = Fence;
	ActiveSlot = ~uint32{0};
}

void FrameResourceRing::Abandon(std::vector<resource::AssetPtr<resource::Asset>> AssetPins) noexcept
{
	if (this->ActiveSlot == ~uint32{0})
		return;
	Slot &Slot = this->Slots[this->ActiveSlot];
	Slot.AssetPins = std::move(AssetPins);
	Slot.Usable = false;
	this->ActiveSlot = ~uint32{0};
}

bool FrameResourceRing::IsAcquired() const noexcept
{
	return ActiveSlot != ~uint32{0};
}
} // namespace renderer
