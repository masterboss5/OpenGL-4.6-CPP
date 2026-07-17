#pragma once

#include <vector>

#include <glm.hpp>

#include "RenderCommand.h"
#include "src/scene/SceneCollection.h"

class Material;

namespace renderer
{
	struct RenderPreparationResult final
	{
		std::vector<PreparedInstance> candidateInstances;
		std::vector<RenderBatch> batches;
		std::vector<RenderCommand> candidateCommands;
		std::vector<const Material*> materials;
	};

	class ScenePreparation final
	{
	public:
		[[nodiscard]] RenderPreparationResult prepare(const SceneCollection& collection, const glm::mat4& viewProjection, uint32 opaquePipelineIndex, uint32 transparentPipelineIndex, bool performFrustumCulling = true) const;
		// Shared conservative sphere/frustum test for the CPU preparation and
		// shadow-view culling paths. The matrix must use the engine's ZO clip
		// convention established by glClipControl.
		[[nodiscard]] static bool intersectsFrustum(const glm::vec4& sphere, const glm::mat4& viewProjection);
	private:
		[[nodiscard]] static uint64 makeSortKey(const RenderItem& item, uint32 pipelineIndex);
		static void radixSort(std::vector<std::pair<uint64, RenderItem>>& items);
	};
}
