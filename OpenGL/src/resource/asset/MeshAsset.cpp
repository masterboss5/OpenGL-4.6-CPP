#include "MeshAsset.h"

#include "src/resource/asset/SkeletonAsset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace resource
{
namespace
{
[[nodiscard]] uint32 FormatSize(MeshVertexFormat Format)
{
	switch (Format)
	{
	case MeshVertexFormat::Float32x2:
		return sizeof(float32) * 2U;
	case MeshVertexFormat::Float32x3:
		return sizeof(float32) * 3U;
	case MeshVertexFormat::Float32x4:
		return sizeof(float32) * 4U;
	case MeshVertexFormat::Float16x2:
		return sizeof(uint16) * 2U;
	case MeshVertexFormat::Float16x4:
		return sizeof(uint16) * 4U;
	case MeshVertexFormat::UNorm8x4:
		return sizeof(uint8) * 4U;
	case MeshVertexFormat::SNorm16x4:
		return sizeof(int16) * 4U;
	case MeshVertexFormat::UInt16x4:
		return sizeof(uint16) * 4U;
	}
	throw std::invalid_argument("Unknown mesh vertex format");
}

void ValidateStream(const MeshVertexStream &Stream)
{
	const uint32 MinimumStride = FormatSize(Stream.Format);
	if (Stream.ElementCount == 0 || Stream.Stride < MinimumStride)
	{
		throw std::invalid_argument("Mesh vertex stream has an invalid element count or stride");
	}
	const uint64 RequiredBytes = static_cast<uint64>(Stream.ElementCount) * Stream.Stride;
	if (RequiredBytes != Stream.Bytes.size())
	{
		throw std::invalid_argument("Mesh vertex stream byte size does not match its declaration");
	}
}
} // namespace

bool Bounds::IsValid() const noexcept
{
	return std::isfinite(this->Minimum.x) && std::isfinite(this->Minimum.y) && std::isfinite(this->Minimum.z) &&
		   std::isfinite(this->Maximum.x) && std::isfinite(this->Maximum.y) && std::isfinite(this->Maximum.z) &&
		   std::isfinite(this->Sphere.x) && std::isfinite(this->Sphere.y) && std::isfinite(this->Sphere.z) &&
		   std::isfinite(this->Sphere.w) && this->Minimum.x <= this->Maximum.x && this->Minimum.y <= this->Maximum.y &&
		   this->Minimum.z <= this->Maximum.z && this->Sphere.w >= 0.0f;
}

MeshAsset::MeshAsset(MeshAssetData Data) : Asset(util::UUID::GenerateRandomUUID()), Data(std::move(Data))
{
	if (this->Data.Name.empty())
		throw std::invalid_argument("Mesh asset requires a non-empty name");
	if (!this->Data.Bounds.IsValid())
		throw std::invalid_argument("Mesh asset bounds are invalid");
	if (this->Data.MaterialSlots.empty())
		throw std::invalid_argument("Mesh asset requires at least one material slot");
	if (this->Data.LODs.empty())
		throw std::invalid_argument("Mesh asset requires at least one LOD");

	std::unordered_set<MaterialSlotID> MaterialIDs;
	for (const MeshMaterialSlot &Slot : this->Data.MaterialSlots)
	{
		if (Slot.ID == 0 || Slot.Name.empty() || !Slot.DefaultMaterial || !MaterialIDs.insert(Slot.ID).second)
		{
			throw std::invalid_argument("Mesh material slots require unique stable IDs, names, and default materials");
		}
	}
	std::unordered_set<MorphTargetID> MorphTargetIDs;
	for (const MorphTargetDefinition &Target : this->Data.MorphTargets)
	{
		if (Target.ID == 0 || Target.Name.empty() || !MorphTargetIDs.insert(Target.ID).second)
		{
			throw std::invalid_argument("Mesh morph targets require unique stable IDs and names");
		}
	}

	float32 PreviousCoverage = std::numeric_limits<float32>::infinity();
	for (uint32 LODIndex = 0; LODIndex < this->Data.LODs.size(); ++LODIndex)
	{
		const MeshLOD &LOD = this->Data.LODs[LODIndex];
		if (LOD.Level != LODIndex || !LOD.Bounds.IsValid() || LOD.Sections.empty() || LOD.VertexStreams.empty())
		{
			throw std::invalid_argument("Mesh LOD declaration is incomplete or out of order");
		}
		if (!std::isfinite(LOD.ScreenCoverage) || LOD.ScreenCoverage < 0.0f || LOD.ScreenCoverage > 1.0f ||
			LOD.ScreenCoverage > PreviousCoverage)
		{
			throw std::invalid_argument("Mesh LOD screen coverage thresholds must be finite and descending");
		}
		PreviousCoverage = LOD.ScreenCoverage;
		uint32 VertexCount = 0;
		for (const MeshVertexStream &Stream : LOD.VertexStreams)
		{
			ValidateStream(Stream);
			if (Stream.Semantic == MeshVertexSemantic::Position)
				VertexCount = Stream.ElementCount;
		}
		if (VertexCount == 0)
			throw std::invalid_argument("Mesh LOD requires a position stream");
		bool HasJointIndices = false;
		bool HasJointWeights = false;
		for (const MeshVertexStream &Stream : LOD.VertexStreams)
		{
			if (Stream.ElementCount != VertexCount)
				throw std::invalid_argument("Mesh LOD vertex streams must have identical element counts");
			HasJointIndices = HasJointIndices || Stream.Semantic == MeshVertexSemantic::JointIndices;
			HasJointWeights = HasJointWeights || Stream.Semantic == MeshVertexSemantic::JointWeights;
		}
		if (HasJointIndices != HasJointWeights)
			throw std::invalid_argument("Mesh LOD joint indices and weights must be declared together");
		const uint32 IndexStride = LOD.IndexStream.Format == MeshIndexFormat::UInt16 ? sizeof(uint16) : sizeof(uint32);
		if (LOD.IndexStream.IndexCount == 0 ||
			static_cast<uint64>(LOD.IndexStream.IndexCount) * IndexStride != LOD.IndexStream.Bytes.size())
		{
			throw std::invalid_argument("Mesh LOD index stream declaration is invalid");
		}
		std::unordered_set<MeshSectionID> SectionIDs;
		for (const MeshSection &Section : LOD.Sections)
		{
			if (Section.ID == 0 || !SectionIDs.insert(Section.ID).second || MaterialIDs.find(Section.MaterialSlot) == MaterialIDs.end() ||
				Section.IndexCount == 0 || static_cast<uint64>(Section.FirstIndex) + Section.IndexCount > LOD.IndexStream.IndexCount ||
				!Section.LocalBounds.IsValid())
			{
				throw std::invalid_argument("Mesh section references invalid geometry or material data");
			}
		}
		std::unordered_set<MorphTargetID> LODMorphTargets;
		for (const MorphTargetLODData &Target : LOD.MorphTargets)
		{
			ValidateStream(Target.PositionDeltas);
			ValidateStream(Target.NormalDeltas);
			if (!MorphTargetIDs.contains(Target.Target) || !LODMorphTargets.insert(Target.Target).second ||
				Target.PositionDeltas.ElementCount != VertexCount || Target.NormalDeltas.ElementCount != VertexCount ||
				Target.PositionDeltas.Format != MeshVertexFormat::Float32x3 || Target.NormalDeltas.Format != MeshVertexFormat::Float32x3)
			{
				throw std::invalid_argument("Mesh LOD morph target data is invalid or inconsistent with the base geometry");
			}
		}
	}
}

const MeshMaterialSlot *MeshAsset::FindMaterialSlot(MaterialSlotID ID) const noexcept
{
	const auto Found = std::find_if(this->Data.MaterialSlots.begin(), this->Data.MaterialSlots.end(),
									[ID](const MeshMaterialSlot &Slot) { return Slot.ID == ID; });
	return Found == this->Data.MaterialSlots.end() ? nullptr : &*Found;
}

bool MeshAsset::HasCPUGeometry(uint32 LOD) const
{
	if (LOD >= this->Data.LODs.size())
		throw std::out_of_range("Requested mesh CPU LOD is out of range");
	return this->Data.LODs[LOD].CPUGeometryResident;
}

void MeshAsset::DiscardCPUGeometry(uint32 LOD)
{
	if (LOD >= this->Data.LODs.size())
		throw std::out_of_range("Requested mesh CPU LOD is out of range");
	if (this->Data.CPURetention == MeshCPURetentionPolicy::RetainAll)
		return;
	MeshLOD &DataLOD = this->Data.LODs[LOD];
	if (!DataLOD.CPUGeometryResident)
		return;
	for (MeshVertexStream &Stream : DataLOD.VertexStreams)
	{
		std::vector<uint8>().swap(Stream.Bytes);
	}
	std::vector<uint8>().swap(DataLOD.IndexStream.Bytes);
	for (MorphTargetLODData &Target : DataLOD.MorphTargets)
	{
		std::vector<uint8>().swap(Target.PositionDeltas.Bytes);
		std::vector<uint8>().swap(Target.NormalDeltas.Bytes);
	}
	DataLOD.CPUGeometryResident = false;
}

SkeletalMeshAsset::SkeletalMeshAsset(MeshAssetData Data, AssetHandle<SkeletonAsset> Skeleton, std::vector<SkinningPartition> Partitions)
	: MeshAsset(std::move(Data)), Skeleton(std::move(Skeleton)), Partitions(std::move(Partitions))
{
	if (!this->Skeleton)
		throw std::invalid_argument("Skeletal mesh requires a skeleton asset handle");
	if (this->Partitions.empty())
		throw std::invalid_argument("Skeletal mesh requires at least one skinning partition");
	for (const SkinningPartition &Partition : this->Partitions)
	{
		if (Partition.IndexCount == 0 || Partition.BonePalette.empty() || Partition.BoneBounds.empty())
		{
			throw std::invalid_argument("Skeletal mesh skinning partitions cannot be empty");
		}
		std::unordered_set<uint32> BoundedJoints;
		for (const SkinningPartition::BoneInfluenceBounds &Bone : Partition.BoneBounds)
		{
			if (!Bone.LocalBounds.IsValid() ||
				std::find(Partition.BonePalette.begin(), Partition.BonePalette.end(), Bone.Joint) == Partition.BonePalette.end() ||
				!BoundedJoints.insert(Bone.Joint).second)
			{
				throw std::invalid_argument("Skeletal mesh partition contains invalid or duplicate bone influence bounds");
			}
		}
	}
}
} // namespace resource
