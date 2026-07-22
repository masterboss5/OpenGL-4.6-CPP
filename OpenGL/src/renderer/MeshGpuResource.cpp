#include "MeshGpuResource.h"

#include "src/pipeline/device/Device.h"
#include "src/renderer/RendererGpuTypes.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace renderer
{
namespace
{
struct AttributeFormat final
{
	VertexAttributeDataType Type = VertexAttributeDataType::Float32;
	uint32 Components = 0;
	bool Normalized = false;
	VertexAttributeInput Input = VertexAttributeInput::FloatingPoint;
};

[[nodiscard]] AttributeFormat TranslateFormat(resource::MeshVertexFormat Format)
{
	switch (Format)
	{
	case resource::MeshVertexFormat::Float32x2:
		return {VertexAttributeDataType::Float32, 2, false, VertexAttributeInput::FloatingPoint};
	case resource::MeshVertexFormat::Float32x3:
		return {VertexAttributeDataType::Float32, 3, false, VertexAttributeInput::FloatingPoint};
	case resource::MeshVertexFormat::Float32x4:
		return {VertexAttributeDataType::Float32, 4, false, VertexAttributeInput::FloatingPoint};
	case resource::MeshVertexFormat::Float16x2:
		return {VertexAttributeDataType::Float16, 2, false, VertexAttributeInput::FloatingPoint};
	case resource::MeshVertexFormat::Float16x4:
		return {VertexAttributeDataType::Float16, 4, false, VertexAttributeInput::FloatingPoint};
	case resource::MeshVertexFormat::UNorm8x4:
		return {VertexAttributeDataType::UInt8, 4, true, VertexAttributeInput::FloatingPoint};
	case resource::MeshVertexFormat::SNorm16x4:
		return {VertexAttributeDataType::Int16, 4, true, VertexAttributeInput::FloatingPoint};
	case resource::MeshVertexFormat::UInt16x4:
		return {VertexAttributeDataType::UInt16, 4, false, VertexAttributeInput::Integer};
	}
	throw std::invalid_argument("Mesh GPU realization encountered an unsupported vertex format");
}

[[nodiscard]] uint32 SemanticLocation(resource::MeshVertexSemantic Semantic, uint32 SemanticIndex)
{
	switch (Semantic)
	{
	case resource::MeshVertexSemantic::Position:
		return 0;
	case resource::MeshVertexSemantic::Normal:
		return 1;
	case resource::MeshVertexSemantic::TextureCoordinate:
		return 2 + SemanticIndex;
	case resource::MeshVertexSemantic::Tangent:
		return 8;
	case resource::MeshVertexSemantic::Color:
		return 9 + SemanticIndex;
	case resource::MeshVertexSemantic::JointIndices:
		return 12;
	case resource::MeshVertexSemantic::JointWeights:
		return 13;
	case resource::MeshVertexSemantic::MorphDelta:
		return 14 + SemanticIndex;
	}
	throw std::invalid_argument("Mesh GPU realization encountered an unsupported vertex semantic");
}

[[nodiscard]] string SemanticName(resource::MeshVertexSemantic Semantic)
{
	switch (Semantic)
	{
	case resource::MeshVertexSemantic::Position:
		return "POSITION";
	case resource::MeshVertexSemantic::Normal:
		return "NORMAL";
	case resource::MeshVertexSemantic::Tangent:
		return "TANGENT";
	case resource::MeshVertexSemantic::TextureCoordinate:
		return "TEXCOORD";
	case resource::MeshVertexSemantic::Color:
		return "COLOR";
	case resource::MeshVertexSemantic::JointIndices:
		return "JOINTS";
	case resource::MeshVertexSemantic::JointWeights:
		return "WEIGHTS";
	case resource::MeshVertexSemantic::MorphDelta:
		return "MORPH";
	}
	return "UNKNOWN";
}
} // namespace

MeshGPUResource::MeshGPUResource(pipeline::device::Device &Device, resource::AssetPtr<resource::MeshAsset> Source)
	: Device(&Device), Source(std::move(Source))
{
	if (this->Source == nullptr)
		throw std::invalid_argument("MeshGPUResource requires a pinned MeshAsset generation");
	(void)this->Device->RequireCurrentContext();
	this->Device->CheckErrors("MeshGPUResource realization precondition");
	this->LODs.resize(this->Source->GetLODs().size());
}

std::unique_ptr<MeshGPULOD> MeshGPUResource::RealizeLOD(uint32 Level)
{
	if (Level >= this->LODs.size())
		throw std::out_of_range("Requested mesh GPU LOD is out of range");
	(void)this->Device->RequireCurrentContext();
	const resource::MeshLOD &SourceLOD = this->Source->GetLODs()[Level];
	if (!SourceLOD.CPUGeometryResident)
		throw std::logic_error("Mesh CPU geometry was discarded before its GPU LOD could be realized");
	auto ResidentLOD = std::make_unique<MeshGPULOD>();
	MeshGPULOD &LOD = *ResidentLOD;
	try
	{
		LOD.Level = SourceLOD.Level;
		LOD.IndexFormat = SourceLOD.IndexStream.Format;
		const auto PositionStream =
			std::find_if(SourceLOD.VertexStreams.begin(), SourceLOD.VertexStreams.end(), [](const resource::MeshVertexStream &Stream)
						 { return Stream.Semantic == resource::MeshVertexSemantic::Position; });
		if (PositionStream == SourceLOD.VertexStreams.end())
			throw std::logic_error("Validated mesh LOD lost its position stream");
		const bool HasJointIndices =
			std::any_of(SourceLOD.VertexStreams.begin(), SourceLOD.VertexStreams.end(), [](const resource::MeshVertexStream &Stream)
						{ return Stream.Semantic == resource::MeshVertexSemantic::JointIndices; });
		const bool HasJointWeights =
			std::any_of(SourceLOD.VertexStreams.begin(), SourceLOD.VertexStreams.end(), [](const resource::MeshVertexStream &Stream)
						{ return Stream.Semantic == resource::MeshVertexSemantic::JointWeights; });
		if (HasJointIndices != HasJointWeights)
			throw std::logic_error("Mesh LOD must provide joint indices and weights together");
		glCreateVertexArrays(1, &LOD.VertexArray);
		LOD.VertexBuffers.resize(SourceLOD.VertexStreams.size() + (HasJointIndices ? 0U : 2U));
		glCreateBuffers(static_cast<GLsizei>(LOD.VertexBuffers.size()), LOD.VertexBuffers.data());
		glCreateBuffers(1, &LOD.IndexBuffer);

		std::vector<VertexBindingDescriptor> Bindings;
		std::vector<VertexAttributeDescriptor> Attributes;
		Bindings.reserve(SourceLOD.VertexStreams.size());
		Attributes.reserve(SourceLOD.VertexStreams.size());
		for (uint32 StreamIndex = 0; StreamIndex < SourceLOD.VertexStreams.size(); ++StreamIndex)
		{
			const resource::MeshVertexStream &Stream = SourceLOD.VertexStreams[StreamIndex];
			if (Stream.Bytes.size() > static_cast<usize>(std::numeric_limits<GLsizeiptr>::max()))
			{
				throw std::overflow_error("Mesh vertex stream exceeds OpenGL buffer limits");
			}
			glNamedBufferStorage(LOD.VertexBuffers[StreamIndex], static_cast<GLsizeiptr>(Stream.Bytes.size()), Stream.Bytes.data(), 0);
			Bindings.push_back({StreamIndex, Stream.Stride, VertexInputRate::PerVertex, 0});
			const AttributeFormat Format = TranslateFormat(Stream.Format);
			Attributes.push_back({SemanticName(Stream.Semantic), Stream.SemanticIndex,
								  SemanticLocation(Stream.Semantic, Stream.SemanticIndex), StreamIndex, Format.Type, Format.Components,
								  Format.Normalized, Format.Input, 0});
		}
		if (!HasJointIndices)
		{
			const uint32 JointBinding = static_cast<uint32>(SourceLOD.VertexStreams.size());
			const uint32 WeightBinding = JointBinding + 1U;
			std::vector<glm::u16vec4> DefaultJoints(PositionStream->ElementCount, glm::u16vec4(0));
			std::vector<glm::vec4> DefaultWeights(PositionStream->ElementCount, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
			glNamedBufferStorage(LOD.VertexBuffers[JointBinding], static_cast<GLsizeiptr>(DefaultJoints.size() * sizeof(glm::u16vec4)),
								 DefaultJoints.data(), 0);
			glNamedBufferStorage(LOD.VertexBuffers[WeightBinding], static_cast<GLsizeiptr>(DefaultWeights.size() * sizeof(glm::vec4)),
								 DefaultWeights.data(), 0);
			Bindings.push_back({JointBinding, sizeof(glm::u16vec4), VertexInputRate::PerVertex, 0});
			Bindings.push_back({WeightBinding, sizeof(glm::vec4), VertexInputRate::PerVertex, 0});
			Attributes.push_back({"JOINTS", 0, SemanticLocation(resource::MeshVertexSemantic::JointIndices, 0), JointBinding,
								  VertexAttributeDataType::UInt16, 4, false, VertexAttributeInput::Integer, 0});
			Attributes.push_back({"WEIGHTS", 0, SemanticLocation(resource::MeshVertexSemantic::JointWeights, 0), WeightBinding,
								  VertexAttributeDataType::Float32, 4, false, VertexAttributeInput::FloatingPoint, 0});
		}
		LOD.MorphVertexCount = PositionStream->ElementCount;
		LOD.MorphTargetCount = static_cast<uint32>(SourceLOD.MorphTargets.size());
		if (LOD.MorphTargetCount != 0)
		{
			std::vector<GPUMorphDeltaRecord> MorphDeltas(static_cast<usize>(LOD.MorphTargetCount) * LOD.MorphVertexCount);
			for (uint32 TargetIndex = 0; TargetIndex < SourceLOD.MorphTargets.size(); ++TargetIndex)
			{
				const resource::MorphTargetLODData &Target = SourceLOD.MorphTargets[TargetIndex];
				if (Target.Target == 0 || !LOD.MorphTargetIndices.emplace(Target.Target, TargetIndex).second ||
					Target.PositionDeltas.Format != resource::MeshVertexFormat::Float32x3 ||
					Target.NormalDeltas.Format != resource::MeshVertexFormat::Float32x3 ||
					Target.PositionDeltas.ElementCount != LOD.MorphVertexCount || Target.NormalDeltas.ElementCount != LOD.MorphVertexCount)
				{
					throw std::logic_error("Mesh morph target GPU data is incompatible with the realized LOD");
				}
				for (uint32 Vertex = 0; Vertex < LOD.MorphVertexCount; ++Vertex)
				{
					glm::vec3 PositionDelta;
					glm::vec3 NormalDelta;
					std::memcpy(&PositionDelta,
								Target.PositionDeltas.Bytes.data() + static_cast<usize>(Vertex) * Target.PositionDeltas.Stride,
								sizeof(glm::vec3));
					std::memcpy(&NormalDelta, Target.NormalDeltas.Bytes.data() + static_cast<usize>(Vertex) * Target.NormalDeltas.Stride,
								sizeof(glm::vec3));
					MorphDeltas[static_cast<usize>(TargetIndex) * LOD.MorphVertexCount + Vertex] = {glm::vec4(PositionDelta, 0.0f),
																									glm::vec4(NormalDelta, 0.0f)};
				}
			}
			glCreateBuffers(1, &LOD.MorphDeltaBuffer);
			glNamedBufferStorage(LOD.MorphDeltaBuffer, static_cast<GLsizeiptr>(MorphDeltas.size() * sizeof(GPUMorphDeltaRecord)),
								 MorphDeltas.data(), 0);
		}
		glNamedBufferStorage(LOD.IndexBuffer, static_cast<GLsizeiptr>(SourceLOD.IndexStream.Bytes.size()),
							 SourceLOD.IndexStream.Bytes.data(), 0);
		LOD.VertexDescriptor = std::make_unique<VertexDescriptor>(Bindings, Attributes);
		LOD.VertexDescriptor->ApplyToVertexArray(*this->Device, LOD.VertexArray);
		for (uint32 StreamIndex = 0; StreamIndex < LOD.VertexBuffers.size(); ++StreamIndex)
		{
			const uint32 Stride = StreamIndex < SourceLOD.VertexStreams.size()
									  ? SourceLOD.VertexStreams[StreamIndex].Stride
									  : (StreamIndex == SourceLOD.VertexStreams.size() ? sizeof(glm::u16vec4) : sizeof(glm::vec4));
			glVertexArrayVertexBuffer(LOD.VertexArray, StreamIndex, LOD.VertexBuffers[StreamIndex], 0, static_cast<GLsizei>(Stride));
		}
		glVertexArrayElementBuffer(LOD.VertexArray, LOD.IndexBuffer);
		this->Device->CheckErrors("MeshGPUResource LOD realization");
	}
	catch (...)
	{
		this->ReleaseLOD(LOD);
		throw;
	}
	if (this->Source->GetCPURetentionPolicy() != resource::MeshCPURetentionPolicy::RetainAll)
	{
		this->Source->DiscardCPUGeometry(Level);
	}
	return ResidentLOD;
}

MeshGPUResource::~MeshGPUResource() noexcept
{
	this->Release();
}

const MeshGPULOD &MeshGPUResource::GetLOD(uint32 Level, uint64 FrameNumber)
{
	if (Level >= this->LODs.size())
		throw std::out_of_range("Requested mesh GPU LOD is out of range");
	if (this->LODs[Level] == nullptr)
		this->LODs[Level] = this->RealizeLOD(Level);
	this->LODs[Level]->LastUsedFrame = FrameNumber;
	return *this->LODs[Level];
}

bool MeshGPUResource::IsLODResident(uint32 Level) const noexcept
{
	return Level < this->LODs.size() && this->LODs[Level] != nullptr;
}

uint32 MeshGPUResource::GetResidentLODCount() const noexcept
{
	return static_cast<uint32>(std::count_if(this->LODs.begin(), this->LODs.end(), [](const auto &LOD) { return LOD != nullptr; }));
}

void MeshGPUResource::CollectLODs(uint64 CompletedFrame, uint32 ProtectedFrameCount)
{
	if (ProtectedFrameCount == 0)
		throw std::invalid_argument("Mesh LOD collection requires at least one protected frame");
	if (this->Source->GetCPURetentionPolicy() != resource::MeshCPURetentionPolicy::RetainAll || CompletedFrame < ProtectedFrameCount)
		return;
	const uint64 OldestProtectedFrame = CompletedFrame - ProtectedFrameCount + 1U;
	for (uint32 Level = 0; Level < this->LODs.size(); ++Level)
	{
		if (this->LODs[Level] != nullptr && this->LODs[Level]->LastUsedFrame < OldestProtectedFrame)
			this->EvictLOD(Level);
	}
}

void MeshGPUResource::EvictLOD(uint32 Level)
{
	if (Level >= this->LODs.size())
		throw std::out_of_range("Requested mesh GPU LOD is out of range");
	if (this->LODs[Level] == nullptr)
		return;
	if (!this->Source->HasCPUGeometry(Level))
		throw std::logic_error("Cannot evict a mesh LOD after its CPU realization data was discarded");
	(void)this->Device->RequireCurrentContext();
	this->ReleaseLOD(*this->LODs[Level]);
	this->LODs[Level].reset();
}

void MeshGPUResource::ReleaseLOD(MeshGPULOD &LOD) noexcept
{
	if (LOD.IndexBuffer != 0)
		glDeleteBuffers(1, &LOD.IndexBuffer);
	if (LOD.MorphDeltaBuffer != 0)
		glDeleteBuffers(1, &LOD.MorphDeltaBuffer);
	if (!LOD.VertexBuffers.empty())
		glDeleteBuffers(static_cast<GLsizei>(LOD.VertexBuffers.size()), LOD.VertexBuffers.data());
	if (LOD.VertexArray != 0)
		glDeleteVertexArrays(1, &LOD.VertexArray);
	LOD.IndexBuffer = 0;
	LOD.MorphDeltaBuffer = 0;
	LOD.VertexArray = 0;
	LOD.VertexBuffers.clear();
	LOD.VertexDescriptor.reset();
}

void MeshGPUResource::Release() noexcept
{
	const bool CanReleaseGPUResources = this->Device != nullptr && this->Device->CanIssueCommands();
	for (auto &LOD : this->LODs)
	{
		if (LOD != nullptr && CanReleaseGPUResources)
			this->ReleaseLOD(*LOD);
	}
	this->LODs.clear();
}

MeshGPUResource &MeshGPUCache::Realize(const resource::AssetPtr<resource::MeshAsset> &Mesh, uint64 FrameNumber)
{
	if (Mesh == nullptr)
		throw std::invalid_argument("MeshGPUCache requires a pinned MeshAsset");
	const util::UUID &Generation = Mesh->GetUUID();
	const auto Found = this->Resources.find(Generation);
	if (Found != this->Resources.end())
	{
		Found->second.LastUsedFrame = FrameNumber;
		return *Found->second.Resource;
	}
	auto Resource = std::make_unique<MeshGPUResource>(*this->Device, Mesh);
	MeshGPUResource &Result = *Resource;
	this->Resources.emplace(Generation, CacheEntry{std::move(Resource), FrameNumber});
	return Result;
}

void MeshGPUCache::Collect(uint64 CompletedFrame, uint32 ProtectedFrameCount)
{
	if (ProtectedFrameCount == 0)
		throw std::invalid_argument("Mesh GPU cache requires at least one protected frame");
	if (CompletedFrame < ProtectedFrameCount)
		return;
	const uint64 OldestProtectedFrame = CompletedFrame - ProtectedFrameCount + 1U;
	for (auto Entry = this->Resources.begin(); Entry != this->Resources.end();)
	{
		Entry->second.Resource->CollectLODs(CompletedFrame, ProtectedFrameCount);
		const auto &Source = Entry->second.Resource->GetSource();
		const bool SupersededGeneration = Source.GetReferenceCount() == 1U;
		const bool CanRecreateFromCPU = Source.GetCPURetentionPolicy() == resource::MeshCPURetentionPolicy::RetainAll;
		if (Entry->second.LastUsedFrame < OldestProtectedFrame && (SupersededGeneration || CanRecreateFromCPU))
			Entry = this->Resources.erase(Entry);
		else
			++Entry;
	}
}

void MeshGPUCache::Clear()
{
	this->Resources.clear();
}
} // namespace renderer
