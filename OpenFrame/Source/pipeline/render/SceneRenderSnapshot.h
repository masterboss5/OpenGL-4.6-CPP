#pragma once

#include "Source/component/object/CObjectAnimationComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectMeshComponent.h"
#include "Source/core/EngineAPI.h"
#include "Source/resource/asset/ModelAsset.h"
#include "Source/scene/DirectionalLightSource.h"
#include "Source/scene/PointLightSource.h"
#include "Source/scene/SpotLightSource.h"
#include "Source/scene/SceneTransformSnapshot.h"

#include <optional>
#include <array>
#include <span>
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
	Light,
	SelectedLight
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
	std::span<const world::ObjectHandle> SelectedObjects;
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
