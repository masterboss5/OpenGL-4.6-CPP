#pragma once

#include "src/resource/Asset.h"
#include "src/resource/asset/AssetHandle.h"
#include "src/resource/asset/MaterialAsset.h"
#include "src/resource/asset/SkeletonAsset.h"

#include <glm.hpp>
#include <span>
#include <vector>

namespace resource
{
using MeshSectionID = uint64;
using MaterialSlotID = uint64;
using MorphTargetID = uint64;

struct Bounds final
{
	glm::vec3 Minimum{0.0f};
	glm::vec3 Maximum{0.0f};
	glm::vec4 Sphere{0.0f, 0.0f, 0.0f, 0.0f};

	[[nodiscard]] bool IsValid() const noexcept;
};

enum class MeshKind : uint8
{
	Static,
	Skeletal
};
enum class MeshCPURetentionPolicy : uint8
{
	RetainAll,
	DiscardAfterRealization,
	RetainCollisionOnly
};
enum class MeshIndexFormat : uint8
{
	UInt16,
	UInt32
};
enum class MeshVertexSemantic : uint8
{
	Position,
	Normal,
	Tangent,
	TextureCoordinate,
	Color,
	JointIndices,
	JointWeights,
	MorphDelta
};
enum class MeshVertexFormat : uint8
{
	Float32x2,
	Float32x3,
	Float32x4,
	Float16x2,
	Float16x4,
	UNorm8x4,
	SNorm16x4,
	UInt16x4
};

enum class MeshSectionFlags : uint32
{
	None = 0,
	CastsShadow = 1U << 0U,
	ReceivesShadow = 1U << 1U,
	Visible = 1U << 2U
};

[[nodiscard]] constexpr MeshSectionFlags operator|(MeshSectionFlags Left, MeshSectionFlags Right) noexcept
{
	return static_cast<MeshSectionFlags>(static_cast<uint32>(Left) | static_cast<uint32>(Right));
}

struct MeshMaterialSlot final
{
	MaterialSlotID ID = 0;
	string Name;
	AssetHandle<MaterialInterfaceAsset> DefaultMaterial;
};

struct MeshVertexStream final
{
	MeshVertexSemantic Semantic = MeshVertexSemantic::Position;
	MeshVertexFormat Format = MeshVertexFormat::Float32x3;
	uint32 SemanticIndex = 0;
	uint32 Stride = 0;
	uint32 ElementCount = 0;
	std::vector<uint8> Bytes;
};

struct MeshIndexStream final
{
	MeshIndexFormat Format = MeshIndexFormat::UInt32;
	uint32 IndexCount = 0;
	std::vector<uint8> Bytes;
};

struct MeshSection final
{
	MeshSectionID ID = 0;
	MaterialSlotID MaterialSlot = 0;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	int32 BaseVertex = 0;
	Bounds LocalBounds;
	MeshSectionFlags Flags = MeshSectionFlags::CastsShadow | MeshSectionFlags::ReceivesShadow | MeshSectionFlags::Visible;
};

struct MorphTargetLODData final
{
	MorphTargetID Target = 0;
	MeshVertexStream PositionDeltas;
	MeshVertexStream NormalDeltas;
};

struct MeshLOD final
{
	uint32 Level = 0;
	float32 ScreenCoverage = 1.0f;
	float32 Hysteresis = 0.02f;
	Bounds Bounds;
	std::vector<MeshVertexStream> VertexStreams;
	MeshIndexStream IndexStream;
	std::vector<MeshSection> Sections;
	std::vector<MorphTargetLODData> MorphTargets;
	bool CPUGeometryResident = true;
};

struct MorphTargetDefinition final
{
	MorphTargetID ID = 0;
	string Name;
};

struct MeshAssetData final
{
	string Name;
	Bounds Bounds;
	MeshCPURetentionPolicy CPURetention = MeshCPURetentionPolicy::DiscardAfterRealization;
	std::vector<MeshMaterialSlot> MaterialSlots;
	std::vector<MeshLOD> LODs;
	std::vector<MorphTargetDefinition> MorphTargets;
	string DerivedDataKey;
};

class MeshAsset : public Asset
{
  public:
	[[nodiscard]] virtual MeshKind GetKind() const noexcept = 0;
	[[nodiscard]] string_view GetName() const noexcept
	{
		return this->Data.Name;
	}
	[[nodiscard]] const Bounds &GetBounds() const noexcept
	{
		return this->Data.Bounds;
	}
	[[nodiscard]] MeshCPURetentionPolicy GetCPURetentionPolicy() const noexcept
	{
		return this->Data.CPURetention;
	}
	[[nodiscard]] std::span<const MeshMaterialSlot> GetMaterialSlots() const noexcept
	{
		return this->Data.MaterialSlots;
	}
	[[nodiscard]] std::span<const MeshLOD> GetLODs() const noexcept
	{
		return this->Data.LODs;
	}
	[[nodiscard]] std::span<const MorphTargetDefinition> GetMorphTargets() const noexcept
	{
		return this->Data.MorphTargets;
	}
	[[nodiscard]] string_view GetDerivedDataKey() const noexcept
	{
		return this->Data.DerivedDataKey;
	}
	[[nodiscard]] const MeshMaterialSlot *FindMaterialSlot(MaterialSlotID ID) const noexcept;
	[[nodiscard]] bool HasCPUGeometry(uint32 LOD) const;
	void DiscardCPUGeometry(uint32 LOD);

  protected:
	explicit MeshAsset(MeshAssetData Data);
	~MeshAsset() override = default;

  private:
	MeshAssetData Data;
};

class StaticMeshAsset final : public MeshAsset
{
  public:
	inline static constexpr AssetType AssetType = AssetType::StaticMesh;
	explicit StaticMeshAsset(MeshAssetData Data) : MeshAsset(std::move(Data))
	{
	}
	[[nodiscard]] MeshKind GetKind() const noexcept override
	{
		return MeshKind::Static;
	}
};

struct SkinningPartition final
{
	struct BoneInfluenceBounds final
	{
		uint32 Joint = 0;
		Bounds LocalBounds;
	};

	uint32 LOD = 0;
	MeshSectionID Section = 0;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	std::vector<uint32> BonePalette;
	std::vector<BoneInfluenceBounds> BoneBounds;
};

class SkeletalMeshAsset final : public MeshAsset
{
  public:
	inline static constexpr AssetType AssetType = AssetType::SkeletalMesh;
	SkeletalMeshAsset(MeshAssetData Data, AssetHandle<SkeletonAsset> Skeleton, std::vector<SkinningPartition> Partitions);
	[[nodiscard]] MeshKind GetKind() const noexcept override
	{
		return MeshKind::Skeletal;
	}
	[[nodiscard]] const AssetHandle<SkeletonAsset> &GetSkeleton() const noexcept
	{
		return this->Skeleton;
	}
	[[nodiscard]] std::span<const SkinningPartition> GetSkinningPartitions() const noexcept
	{
		return this->Partitions;
	}

  private:
	AssetHandle<SkeletonAsset> Skeleton;
	std::vector<SkinningPartition> Partitions;
};
} // namespace resource
