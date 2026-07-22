#pragma once

#include "RenderCommand.h"
#include "src/scene/SceneCollection.h"

#include <glm.hpp>
#include <vector>

namespace renderer
{
struct RenderPreparationResult final
{
	std::vector<PreparedInstance> CandidateInstances;
	std::vector<RenderBatch> Batches;
	std::vector<RenderCommand> CandidateCommands;
	std::vector<GPUMaterialRecord> Materials;
};

class ScenePreparation final
{
  public:
	[[nodiscard]] RenderPreparationResult Prepare(const SceneCollection &Collection, const glm::mat4 &ViewProjection,
												  uint32 OpaquePipelineIndex, uint32 TransparentPipelineIndex,
												  bool PerformFrustumCulling = true, bool ShadowCastersOnly = false) const;
	// Shared conservative sphere/frustum test for the CPU preparation and
	// shadow-view culling paths. The matrix must use the engine's ZO clip
	// convention established by glClipControl.
	[[nodiscard]] static bool IntersectsFrustum(const glm::vec4 &Sphere, const glm::mat4 &ViewProjection);

  private:
	static void RadixSort(std::vector<RenderItem> &Items, uint32 OpaquePipelineIndex, uint32 TransparentPipelineIndex);
};
} // namespace renderer
