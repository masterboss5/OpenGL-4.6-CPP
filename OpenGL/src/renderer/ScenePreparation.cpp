#include "ScenePreparation.h"

#include <array>
#include <cstring>
#include <unordered_map>

#include "StaticMesh.h"

namespace renderer
{
	namespace
	{
		[[nodiscard]] glm::vec4 normalizedPlane(glm::vec4 plane)
		{
			const float32 length = glm::length(glm::vec3(plane));
			return length > 0.0f ? plane / length : plane;
		}
	}

	bool ScenePreparation::intersectsFrustum(const glm::vec4& sphere, const glm::mat4& matrix)
	{
		const glm::vec4 row0 { matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0] };
		const glm::vec4 row1 { matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1] };
		const glm::vec4 row2 { matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2] };
		const glm::vec4 row3 { matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3] };
		const std::array<glm::vec4, 6> planes { normalizedPlane(row3 + row0), normalizedPlane(row3 - row0), normalizedPlane(row3 + row1), normalizedPlane(row3 - row1), normalizedPlane(row2), normalizedPlane(row3 - row2) };
		for (const glm::vec4& plane : planes)
		{
			if (glm::dot(glm::vec3(plane), glm::vec3(sphere)) + plane.w < -sphere.w) return false;
		}
		return true;
	}

	uint64 ScenePreparation::makeSortKey(const RenderItem& item, uint32 pipelineIndex)
	{
		const uint64 pass = item.transparent ? 1ULL : 0ULL;
		const uint64 vertexArray = static_cast<uint64>(item.mesh->getVAO()) & 0x00FFFFFFULL;
		return (pass << 63U) | ((static_cast<uint64>(pipelineIndex) & 0x7FFFFFULL) << 40U) | (vertexArray << 16U) | (static_cast<uint64>(item.mesh->getIndicesCount()) & 0xFFFFULL);
	}

	void ScenePreparation::radixSort(std::vector<std::pair<uint64, RenderItem>>& items)
	{
		if (items.empty()) return;
		std::vector<std::pair<uint64, RenderItem>> scratch(items.size());
		for (uint32 byte = 0; byte < sizeof(uint64); ++byte)
		{
			std::array<uint32, 256> counts {};
			for (const auto& item : items) ++counts[(item.first >> (byte * 8U)) & 0xFFU];
			uint32 running = 0;
			for (uint32& count : counts) { const uint32 prior = count; count = running; running += prior; }
			for (const auto& item : items) scratch[counts[(item.first >> (byte * 8U)) & 0xFFU]++] = item;
			items.swap(scratch);
		}
	}

	RenderPreparationResult ScenePreparation::prepare(const SceneCollection& collection, const glm::mat4& viewProjection, uint32 opaquePipelineIndex, uint32 transparentPipelineIndex, bool performFrustumCulling) const
	{
		std::vector<std::pair<uint64, RenderItem>> visible;
		visible.reserve(collection.getRenderItems().size());
		for (const RenderItem& item : collection.getRenderItems())
		{
			if (!performFrustumCulling || intersectsFrustum(item.worldBounds, viewProjection)) visible.emplace_back(makeSortKey(item, item.transparent ? transparentPipelineIndex : opaquePipelineIndex), item);
		}
		radixSort(visible);

		RenderPreparationResult result;
		result.candidateInstances.reserve(visible.size());
		std::unordered_map<const Material*, uint32> materialIndices;
		for (std::size_t index = 0; index < visible.size();)
		{
			const uint64 key = visible[index].first;
			const RenderItem& first = visible[index].second;
			const uint32 firstCandidate = static_cast<uint32>(result.candidateInstances.size());
			const uint32 pipelineIndex = first.transparent ? transparentPipelineIndex : opaquePipelineIndex;
			std::size_t end = index;
			while (end < visible.size() && visible[end].first == key) ++end;
			const uint32 batchIndex = static_cast<uint32>(result.batches.size());
			for (std::size_t current = index; current < end; ++current)
			{
				const RenderItem& item = visible[current].second;
				const Material* material = &item.mesh->getMaterial();
				auto [iterator, inserted] = materialIndices.emplace(material, static_cast<uint32>(result.materials.size()));
				if (inserted) result.materials.push_back(material);
				result.candidateInstances.push_back({ .transform = item.transform, .previousTransform = item.transform, .worldBounds = item.worldBounds, .materialIndex = iterator->second, .objectID = item.objectID, .batchIndex = batchIndex, .flags = item.transparent ? 1U : 0U });
			}
			const uint32 candidateCount = static_cast<uint32>(end - index);
			result.batches.push_back({ .passClass = first.transparent ? RenderPassClass::Transparency : RenderPassClass::GBuffer, .vertexDescriptor = &first.mesh->getVertexDescriptor(), .pipelineIndex = pipelineIndex, .vertexArray = first.mesh->getVAO(), .indexCount = first.mesh->getIndicesCount(), .firstIndex = 0, .baseVertex = 0, .firstCandidate = firstCandidate, .candidateCount = candidateCount });
			result.candidateCommands.push_back({ .indexCount = first.mesh->getIndicesCount(), .instanceCount = candidateCount, .firstIndex = 0, .baseVertex = 0, .baseInstance = firstCandidate });
			index = end;
		}
		return result;
	}
}
