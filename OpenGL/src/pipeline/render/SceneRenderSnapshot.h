#pragma once

#include "src/component/object/CObjectAnimationComponent.h"
#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/core/EngineAPI.h"
#include "src/resource/asset/ModelAsset.h"
#include "src/scene/DirectionalLightSource.h"
#include "src/scene/PointLightSource.h"
#include "src/scene/SpotLightSource.h"
#include "src/scene/SceneTransformSnapshot.h"

#include <optional>
#include <array>
#include <unordered_map>
#include <vector>

namespace world
{
class Scene;
}

namespace pipeline::render
{
struct SceneAnimationSnapshot final
{
	std::vector<components::AnimationRigRuntimeState> RigStates;
	std::vector<components::AnimationMorphWeight> MorphWeights;
};

struct SceneMeshSnapshot final
{
	world::ObjectHandle Owner;
	resource::AssetPtr<resource::ModelAsset> Model;
	glm::mat4 ObjectTransform{1.0f};
	components::MeshLODPolicy LODPolicy;
	std::vector<components::MeshMaterialOverride> MaterialOverrides;
	components::MeshVisibilityFlags Visibility = components::MeshVisibilityFlags::None;
	components::ObjectMobility Mobility = components::ObjectMobility::Movable;
	uint32 RenderLayerMask = 0;
	uint64 ModelPublishedGeneration = 0;
	std::optional<SceneAnimationSnapshot> Animation;
};

enum class SceneDebugLineCategory : uint8
{
	Bounds,
	Skeleton,
	Camera,
	Light
};

struct SceneDebugLine final
{
	glm::vec3 Start{0.0f};
	glm::vec3 End{0.0f};
	glm::vec4 Color{1.0f};
	SceneDebugLineCategory Category = SceneDebugLineCategory::Bounds;
};

struct SceneDebugBounds final
{
	world::ObjectHandle Owner;
	std::array<glm::vec3, 8> Corners{};
};

struct SceneRenderSnapshot final
{
	uint64 SceneID = 0;
	uint32 ObjectCount = 0;
	std::vector<SceneMeshSnapshot> Meshes;
	std::vector<DirectionalLightSource> DirectionalLights;
	std::vector<PointLightSource> PointLights;
	std::vector<SpotLightSource> SpotLights;
	std::vector<SceneDebugLine> DebugLines;
	std::vector<SceneDebugBounds> DebugBounds;
};

// The render-graph contract names this immutable CPU packet RenderSceneSnapshot;
// retain the established SceneRenderSnapshot spelling as the canonical type.
using RenderSceneSnapshot = SceneRenderSnapshot;

struct SceneRenderSnapshotBuildOptions final
{
	bool RespectEditorVisibility = false;
	bool IncludeBounds = false;
	bool IncludeSkeletons = false;
	bool IncludeCameras = false;
	bool IncludeLights = false;
};

struct SceneRenderSnapshotBuildScratch final
{
	struct ObjectIndexEntry final
	{
		uint32 Index = 0;
		uint64 Generation = 0;
	};

	world::SceneTransformSnapshot WorldTransforms;
	world::SceneTransformSnapshotBuildScratch WorldTransformScratch;
	std::vector<world::ObjectHandle> Objects;
	std::vector<world::ObjectHandle> Parents;
	std::vector<bool> LocalVisible;
	std::vector<uint8> VisibilityStates;
	std::unordered_map<world::ObjectHandle, ObjectIndexEntry, world::ObjectHandleHash> Indices;
	std::vector<uint32> VisibilityChain;
	std::vector<glm::mat4> SkeletonNodeTransforms;
	std::vector<glm::mat4> SkeletonReferenceGlobal;
	std::vector<glm::vec3> SkeletonJointPositions;
	uint64 IndexGeneration = 0;
};

class ENGINE_API SceneRenderSnapshotBuilder final
{
  public:
	[[nodiscard]] static SceneRenderSnapshot Build(const world::Scene &Scene, SceneRenderSnapshotBuildOptions Options = {});
	static void BuildInto(const world::Scene &Scene, SceneRenderSnapshot &Output, SceneRenderSnapshotBuildOptions Options = {});
	static void BuildInto(const world::Scene &Scene, SceneRenderSnapshot &Output, SceneRenderSnapshotBuildOptions Options,
						  SceneRenderSnapshotBuildScratch &Scratch);
};
} // namespace pipeline::render
