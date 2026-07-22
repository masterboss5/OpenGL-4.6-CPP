#pragma once

#include "src/pipeline/vertex/VertexDescriptor.h"
#include "src/resource/asset/MeshAsset.h"

#include <GL/glew.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pipeline::device
{
class Device;
}

namespace renderer
{
struct MeshGPULOD final
{
	uint32 Level = 0;
	GLuint VertexArray = 0;
	std::vector<GLuint> VertexBuffers;
	GLuint IndexBuffer = 0;
	GLuint MorphDeltaBuffer = 0;
	uint32 MorphVertexCount = 0;
	uint32 MorphTargetCount = 0;
	uint64 LastUsedFrame = 0;
	resource::MeshIndexFormat IndexFormat = resource::MeshIndexFormat::UInt32;
	std::unique_ptr<VertexDescriptor> VertexDescriptor;
	std::unordered_map<resource::MorphTargetID, uint32> MorphTargetIndices;
};

class MeshGPUResource final
{
  public:
	MeshGPUResource(pipeline::device::Device &Device, resource::AssetPtr<resource::MeshAsset> Source);
	~MeshGPUResource() noexcept;
	MeshGPUResource(const MeshGPUResource &) = delete;
	MeshGPUResource &operator=(const MeshGPUResource &) = delete;
	MeshGPUResource(MeshGPUResource &&) = delete;
	MeshGPUResource &operator=(MeshGPUResource &&) = delete;

	[[nodiscard]] const resource::MeshAsset &GetSource() const noexcept
	{
		return *this->Source;
	}
	[[nodiscard]] const MeshGPULOD &GetLOD(uint32 Level, uint64 FrameNumber);
	[[nodiscard]] bool IsLODResident(uint32 Level) const noexcept;
	[[nodiscard]] uint32 GetLODCount() const noexcept
	{
		return static_cast<uint32>(this->LODs.size());
	}
	[[nodiscard]] uint32 GetResidentLODCount() const noexcept;
	void CollectLODs(uint64 CompletedFrame, uint32 ProtectedFrameCount);

  private:
	pipeline::device::Device *Device = nullptr;
	resource::AssetPtr<resource::MeshAsset> Source;
	std::vector<std::unique_ptr<MeshGPULOD>> LODs;
	[[nodiscard]] std::unique_ptr<MeshGPULOD> RealizeLOD(uint32 Level);
	void EvictLOD(uint32 Level);
	void ReleaseLOD(MeshGPULOD &LOD) noexcept;
	void Release() noexcept;
};

class MeshGPUCache final
{
  public:
	explicit MeshGPUCache(pipeline::device::Device &Device) : Device(&Device)
	{
	}
	~MeshGPUCache() = default;
	MeshGPUCache(const MeshGPUCache &) = delete;
	MeshGPUCache &operator=(const MeshGPUCache &) = delete;

	[[nodiscard]] MeshGPUResource &Realize(const resource::AssetPtr<resource::MeshAsset> &Mesh, uint64 FrameNumber);
	void Collect(uint64 CompletedFrame, uint32 ProtectedFrameCount);
	void Clear();

  private:
	pipeline::device::Device *Device = nullptr;
	struct CacheEntry final
	{
		std::unique_ptr<MeshGPUResource> Resource;
		uint64 LastUsedFrame = 0;
	};
	std::unordered_map<util::UUID, CacheEntry> Resources;
};
} // namespace renderer
