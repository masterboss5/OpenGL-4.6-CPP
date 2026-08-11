#include "src/pipeline/render/ScenePreparation.h"

#include "src/pipeline/vertex/VertexDescriptor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pipeline::render
{
namespace
{
[[nodiscard]] bool IsFinite(const glm::vec4 &Value) noexcept
{
	return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) && std::isfinite(Value.w);
}

[[nodiscard]] glm::vec4 NormalizedPlane(glm::vec4 Plane)
{
	const glm::vec3 AbsoluteNormal = glm::abs(glm::vec3(Plane));
	const float32 MaximumComponent = std::max({AbsoluteNormal.x, AbsoluteNormal.y, AbsoluteNormal.z});
	if (!std::isfinite(MaximumComponent) || MaximumComponent <= std::numeric_limits<float32>::min() || !std::isfinite(Plane.w))
		throw std::invalid_argument("Frustum contains a non-finite or degenerate plane");
	const glm::vec3 ScaledNormal = glm::vec3(Plane) / MaximumComponent;
	const float32 ScaledLength = glm::length(ScaledNormal);
	if (!std::isfinite(ScaledLength) || ScaledLength <= std::numeric_limits<float32>::epsilon())
		throw std::invalid_argument("Frustum contains a non-normalizable plane");
	const float32 Length = MaximumComponent * ScaledLength;
	const glm::vec4 Result = Plane / Length;
	if (!IsFinite(Result))
		throw std::invalid_argument("Frustum normalization produced a non-finite plane");
	return Result;
}
} // namespace

bool ScenePreparation::IntersectsFrustum(const glm::vec4 &Sphere, const glm::mat4 &Matrix)
{
	if (!IsFinite(Sphere) || Sphere.w < 0.0f)
		throw std::invalid_argument("Bounding sphere must be finite with a non-negative radius");
	for (uint32 Column = 0; Column < 4; ++Column)
	{
		if (!IsFinite(Matrix[Column]))
			throw std::invalid_argument("View-projection matrix must be finite");
	}
	const glm::vec4 Row0{Matrix[0][0], Matrix[1][0], Matrix[2][0], Matrix[3][0]};
	const glm::vec4 Row1{Matrix[0][1], Matrix[1][1], Matrix[2][1], Matrix[3][1]};
	const glm::vec4 Row2{Matrix[0][2], Matrix[1][2], Matrix[2][2], Matrix[3][2]};
	const glm::vec4 Row3{Matrix[0][3], Matrix[1][3], Matrix[2][3], Matrix[3][3]};
	const std::array<glm::vec4, 6> Planes{NormalizedPlane(Row3 + Row0), NormalizedPlane(Row3 - Row0), NormalizedPlane(Row3 + Row1),
										  NormalizedPlane(Row3 - Row1), NormalizedPlane(Row2),		  NormalizedPlane(Row3 - Row2)};
	for (const glm::vec4 &Plane : Planes)
	{
		if (glm::dot(glm::vec3(Plane), glm::vec3(Sphere)) + Plane.w < -Sphere.w)
			return false;
	}
	return true;
}

ScenePreparation::ScenePreparation(const uint32 Capacity) : Capacity(Capacity)
{
	if (Capacity == 0)
		throw std::invalid_argument("Scene preparation capacity must be non-zero");
}

ScenePreparation::Workspace::Workspace(const uint32 Capacity)
{
	if (Capacity == 0)
		throw std::invalid_argument("Scene preparation workspace capacity must be non-zero");
	this->SortScratch.reserve(Capacity);
	this->Visible.reserve(Capacity);
	this->MaterialIndices.reserve(Capacity);
}

void ScenePreparation::RadixSort(std::vector<RenderItem> &Items, std::vector<RenderItem> &SortScratch, const uint32 OpaquePipelineIndex,
								 const uint32 TransparentPipelineIndex) const
{
	if (Items.empty())
		return;
	if (Items.size() > this->Capacity)
		throw std::overflow_error("Scene preparation item capacity exceeded");
	SortScratch.resize(Items.size());
	const auto RadixField = [&Items, &Scratch = SortScratch](const auto &Selector)
	{
		for (uint32 Byte = 0; Byte < sizeof(uint32); ++Byte)
		{
			std::array<uint32, 256> Counts{};
			for (const RenderItem &Item : Items)
				++Counts[(Selector(Item) >> (Byte * 8U)) & 0xFFU];
			uint32 Running = 0;
			for (uint32 &Count : Counts)
			{
				const uint32 Prior = Count;
				Count = Running;
				Running += Prior;
			}
			for (const RenderItem &Item : Items)
				Scratch[Counts[(Selector(Item) >> (Byte * 8U)) & 0xFFU]++] = Item;
			Items.swap(Scratch);
		}
	};
	RadixField([](const RenderItem &Item) { return Item.MorphVertexCount; });
	RadixField([](const RenderItem &Item) { return Item.MorphDeltaBuffer; });
	RadixField([](const RenderItem &Item) { return static_cast<uint32>(Item.BaseVertex) ^ 0x80000000U; });
	RadixField([](const RenderItem &Item) { return Item.IndexCount; });
	RadixField([](const RenderItem &Item) { return Item.FirstIndex; });
	RadixField([](const RenderItem &Item) { return static_cast<uint32>(Item.IndexFormat); });
	const auto VertexDescriptorKey = [](const RenderItem &Item)
	{ return Item.VertexDescriptor != nullptr ? Item.VertexDescriptor->GetLayoutHash() : uint64{0}; };
	RadixField([VertexDescriptorKey](const RenderItem &Item) { return static_cast<uint32>(VertexDescriptorKey(Item)); });
	RadixField([VertexDescriptorKey](const RenderItem &Item) { return static_cast<uint32>(VertexDescriptorKey(Item) >> 32U); });
	RadixField([](const RenderItem &Item) { return Item.VertexArray; });
	RadixField([](const RenderItem &Item) { return Item.TwoSided ? 1U : 0U; });
	RadixField([OpaquePipelineIndex, TransparentPipelineIndex](const RenderItem &Item)
			   { return Item.Transparent ? TransparentPipelineIndex : OpaquePipelineIndex; });
	RadixField([](const RenderItem &Item) { return Item.Transparent ? 1U : 0U; });
}

RenderPreparationResult ScenePreparation::Prepare(const SceneCollection &Collection, const glm::mat4 &ViewProjection,
												  const uint32 OpaquePipelineIndex, const uint32 TransparentPipelineIndex,
												  const bool PerformFrustumCulling, const bool ShadowCastersOnly) const
{
	RenderPreparationResult Result;
	if (Collection.GetRenderItems().size() > std::numeric_limits<uint32>::max())
		throw std::overflow_error("Scene preparation item count exceeds the engine index range");
	ScenePreparation Scratch(static_cast<uint32>(std::max<usize>(Collection.GetRenderItems().size(), 1)));
	Workspace Work(static_cast<uint32>(std::max<usize>(Collection.GetRenderItems().size(), 1)));
	Scratch.PrepareInto(Collection, ViewProjection, OpaquePipelineIndex, TransparentPipelineIndex, Work, Result, PerformFrustumCulling,
						ShadowCastersOnly);
	return Result;
}

void ScenePreparation::PrepareInto(const SceneCollection &Collection, const glm::mat4 &ViewProjection, const uint32 OpaquePipelineIndex,
								   const uint32 TransparentPipelineIndex, Workspace &Scratch, RenderPreparationResult &Result,
								   const bool PerformFrustumCulling, const bool ShadowCastersOnly)
{
	if (Collection.GetRenderItems().size() > this->Capacity)
		throw std::overflow_error("Scene preparation item capacity exceeded");
	Result.CandidateInstances.clear();
	Result.Batches.clear();
	Result.CandidateCommands.clear();
	Result.Materials.clear();
	Scratch.MaterialIndices.clear();
	for (const RenderItem &Item : Collection.GetRenderItems())
	{
		auto [Iterator, Inserted] = Scratch.MaterialIndices.try_emplace(Item.MaterialGeneration);
		if (Inserted)
		{
			Iterator->second = static_cast<uint32>(Result.Materials.size());
			Result.Materials.push_back(Item.Material);
		}
	}
	Scratch.Visible.clear();
	for (const RenderItem &Item : Collection.GetRenderItems())
	{
		if ((!ShadowCastersOnly || Item.CastsShadows) && (!PerformFrustumCulling || IntersectsFrustum(Item.WorldBounds, ViewProjection)))
			Scratch.Visible.push_back(Item);
	}
	this->RadixSort(Scratch.Visible, Scratch.SortScratch, OpaquePipelineIndex, TransparentPipelineIndex);

	for (usize Index = 0; Index < Scratch.Visible.size();)
	{
		const RenderItem &First = Scratch.Visible[Index];
		const uint32 FirstCandidate = static_cast<uint32>(Result.CandidateInstances.size());
		const uint32 PipelineIndex = First.Transparent ? TransparentPipelineIndex : OpaquePipelineIndex;
		usize End = Index;
		while (End < Scratch.Visible.size() && Scratch.Visible[End].Transparent == First.Transparent &&
			   Scratch.Visible[End].TwoSided == First.TwoSided && Scratch.Visible[End].VertexDescriptor == First.VertexDescriptor &&
			   Scratch.Visible[End].VertexArray == First.VertexArray && Scratch.Visible[End].FirstIndex == First.FirstIndex &&
			   Scratch.Visible[End].IndexCount == First.IndexCount && Scratch.Visible[End].BaseVertex == First.BaseVertex &&
			   Scratch.Visible[End].MorphDeltaBuffer == First.MorphDeltaBuffer &&
			   Scratch.Visible[End].MorphVertexCount == First.MorphVertexCount && Scratch.Visible[End].IndexFormat == First.IndexFormat)
			++End;
		const uint32 BatchIndex = static_cast<uint32>(Result.Batches.size());
		for (usize Current = Index; Current < End; ++Current)
		{
			const RenderItem &Item = Scratch.Visible[Current];
			const auto Iterator = Scratch.MaterialIndices.find(Item.MaterialGeneration);
			if (Iterator == Scratch.MaterialIndices.end())
				throw std::logic_error("Prepared material identity disappeared from the deterministic material table");
			uint32 Flags = 0;
			Flags |= Item.Transparent ? static_cast<uint32>(GPUInstanceFlag::Transparent) : 0U;
			Flags |= Item.Skinned ? static_cast<uint32>(GPUInstanceFlag::Skinned) : 0U;
			Flags |= Item.MorphWeightCount != 0 ? static_cast<uint32>(GPUInstanceFlag::Morphed) : 0U;
			Flags |= Item.CastsShadows ? static_cast<uint32>(GPUInstanceFlag::CastsShadows) : 0U;
			Flags |= Item.ReceivesShadows ? static_cast<uint32>(GPUInstanceFlag::ReceivesShadows) : 0U;
			Flags |= Item.Masked ? static_cast<uint32>(GPUInstanceFlag::Masked) : 0U;
			Flags |= Item.TwoSided ? static_cast<uint32>(GPUInstanceFlag::TwoSided) : 0U;
			Result.CandidateInstances.push_back({.Transform = Item.Transform,
												 .PreviousTransform = Item.PreviousTransform,
												 .WorldBounds = Item.WorldBounds,
												 .MaterialIndex = Iterator->second,
												 .ObjectID = Item.ObjectID,
												 .BatchIndex = BatchIndex,
												 .SkinPaletteOffset = Item.SkinPaletteOffset,
												 .PreviousSkinPaletteOffset = Item.PreviousSkinPaletteOffset,
												 .Flags = Flags,
												 .MorphWeightOffset = Item.MorphWeightOffset,
												 .MorphWeightCount = Item.MorphWeightCount});
		}
		const uint32 CandidateCount = static_cast<uint32>(End - Index);
		Result.Batches.push_back({.PassClass = First.Transparent ? RenderPassClass::Transparency : RenderPassClass::GBuffer,
								  .VertexDescriptor = First.VertexDescriptor,
								  .PipelineIndex = PipelineIndex,
								  .VertexArray = First.VertexArray,
								  .IndexCount = First.IndexCount,
								  .FirstIndex = First.FirstIndex,
								  .BaseVertex = First.BaseVertex,
								  .FirstCandidate = FirstCandidate,
								  .CandidateCount = CandidateCount,
								  .IndexFormat = First.IndexFormat,
								  .MorphDeltaBuffer = First.MorphDeltaBuffer,
								  .MorphVertexCount = First.MorphVertexCount,
								  .TwoSided = First.TwoSided});
		Result.CandidateCommands.push_back({.IndexCount = First.IndexCount,
											.InstanceCount = CandidateCount,
											.FirstIndex = First.FirstIndex,
											.BaseVertex = First.BaseVertex,
											.BaseInstance = FirstCandidate});
		Index = End;
	}
	return;
}
} // namespace pipeline::render
