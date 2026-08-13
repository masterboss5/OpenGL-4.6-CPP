#pragma once

#include "Source/core/EngineAPI.h"

#include "RenderCommand.h"
#include "Source/scene/SceneCollection.h"

#include <glm.hpp>
#include <unordered_map>
#include <vector>

namespace pipeline::render
{
struct RenderPreparationResult final
{
	std::vector<PreparedInstance> CandidateInstances;
	std::vector<RenderBatch> Batches;
	std::vector<RenderCommand> CandidateCommands;
	std::vector<GPUMaterialRecord> Materials;

	void Reserve(const uint32 Capacity)
	{
		this->CandidateInstances.reserve(Capacity);
		this->Batches.reserve(Capacity);
		this->CandidateCommands.reserve(Capacity);
		this->Materials.reserve(Capacity);
	}
};

class ENGINE_API ScenePreparation final
{
  public:
	struct Workspace final
	{
		explicit Workspace(uint32 Capacity = 4'096);

		std::vector<RenderItem> SortScratch;
		std::vector<RenderItem> Visible;
		std::unordered_map<util::UUID, uint32> MaterialIndices;
	};

	explicit ScenePreparation(uint32 Capacity = 4'096);

	[[nodiscard]] RenderPreparationResult Prepare(const SceneCollection &Collection, const glm::mat4 &ViewProjection,
												  uint32 OpaquePipelineIndex, uint32 TransparentPipelineIndex,
												  bool PerformFrustumCulling = true, bool ShadowCastersOnly = false) const;
	void PrepareInto(const SceneCollection &Collection, const glm::mat4 &ViewProjection, uint32 OpaquePipelineIndex,
					 uint32 TransparentPipelineIndex, Workspace &Scratch, RenderPreparationResult &Result,
					 bool PerformFrustumCulling = true, bool ShadowCastersOnly = false);
	// Shared conservative sphere/frustum test for the CPU preparation and
	// shadow-view culling paths. The matrix must use the engine's ZO clip
	// convention established by glClipControl.
	[[nodiscard]] static bool IntersectsFrustum(const glm::vec4 &Sphere, const glm::mat4 &ViewProjection);

  private:
	void RadixSort(std::vector<RenderItem> &Items, std::vector<RenderItem> &SortScratch, uint32 OpaquePipelineIndex,
				   uint32 TransparentPipelineIndex) const;

	uint32 Capacity = 0;
};
} // namespace pipeline::render
