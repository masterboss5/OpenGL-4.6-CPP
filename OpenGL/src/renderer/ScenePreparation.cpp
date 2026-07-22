#include "ScenePreparation.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace renderer
{
namespace
{
[[nodiscard]] glm::vec4 NormalizedPlane(glm::vec4 Plane)
{
	const float32 Length = glm::length(glm::vec3(Plane));
	return Length > 0.0f ? Plane / Length : Plane;
}
} // namespace

bool ScenePreparation::IntersectsFrustum(const glm::vec4 &Sphere, const glm::mat4 &Matrix)
{
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

void ScenePreparation::RadixSort(std::vector<RenderItem> &Items, uint32 OpaquePipelineIndex, uint32 TransparentPipelineIndex)
{
	if (Items.empty())
		return;
	std::vector<RenderItem> Scratch(Items.size());
	const auto RadixField = [&Items, &Scratch](const auto &Selector)
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
	RadixField([](const RenderItem &Item) { return Item.VertexArray; });
	RadixField([](const RenderItem &Item) { return Item.TwoSided ? 1U : 0U; });
	RadixField([OpaquePipelineIndex, TransparentPipelineIndex](const RenderItem &Item)
			   { return Item.Transparent ? TransparentPipelineIndex : OpaquePipelineIndex; });
	RadixField([](const RenderItem &Item) { return Item.Transparent ? 1U : 0U; });
}

RenderPreparationResult ScenePreparation::Prepare(const SceneCollection &Collection, const glm::mat4 &ViewProjection,
												  uint32 OpaquePipelineIndex, uint32 TransparentPipelineIndex, bool PerformFrustumCulling,
												  bool ShadowCastersOnly) const
{
	RenderPreparationResult Result;
	std::unordered_map<util::UUID, uint32> MaterialIndices;
	for (const RenderItem &Item : Collection.GetRenderItems())
	{
		auto [Iterator, Inserted] = MaterialIndices.emplace(Item.MaterialGeneration, static_cast<uint32>(Result.Materials.size()));
		(void)Iterator;
		if (Inserted)
			Result.Materials.push_back(Item.Material);
	}
	std::vector<RenderItem> Visible;
	Visible.reserve(Collection.GetRenderItems().size());
	for (const RenderItem &Item : Collection.GetRenderItems())
	{
		if ((!ShadowCastersOnly || Item.CastsShadows) && (!PerformFrustumCulling || IntersectsFrustum(Item.WorldBounds, ViewProjection)))
			Visible.push_back(Item);
	}
	RadixSort(Visible, OpaquePipelineIndex, TransparentPipelineIndex);

	Result.CandidateInstances.reserve(Visible.size());
	for (usize Index = 0; Index < Visible.size();)
	{
		const RenderItem &First = Visible[Index];
		const uint32 FirstCandidate = static_cast<uint32>(Result.CandidateInstances.size());
		const uint32 PipelineIndex = First.Transparent ? TransparentPipelineIndex : OpaquePipelineIndex;
		usize End = Index;
		while (End < Visible.size() && Visible[End].Transparent == First.Transparent && Visible[End].TwoSided == First.TwoSided &&
			   Visible[End].VertexArray == First.VertexArray && Visible[End].FirstIndex == First.FirstIndex &&
			   Visible[End].IndexCount == First.IndexCount && Visible[End].BaseVertex == First.BaseVertex &&
			   Visible[End].MorphDeltaBuffer == First.MorphDeltaBuffer && Visible[End].MorphVertexCount == First.MorphVertexCount &&
			   Visible[End].IndexFormat == First.IndexFormat)
			++End;
		const uint32 BatchIndex = static_cast<uint32>(Result.Batches.size());
		for (usize Current = Index; Current < End; ++Current)
		{
			const RenderItem &Item = Visible[Current];
			const auto Iterator = MaterialIndices.find(Item.MaterialGeneration);
			if (Iterator == MaterialIndices.end())
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
	return Result;
}
} // namespace renderer
