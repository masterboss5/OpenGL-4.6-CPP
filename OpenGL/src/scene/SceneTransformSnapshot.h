#pragma once

#include "Scene.h"

#include <unordered_map>
#include <vector>

#include <glm.hpp>

namespace world
{
enum class SceneTransformResolutionState : uint8
{
	Unvisited,
	Visiting,
	Resolved
};

struct SceneTransformSource final
{
	ObjectHandle Object;
	ObjectHandle Parent;
	glm::mat4 Local{1.0f};
	glm::mat4 World{1.0f};
	SceneTransformResolutionState State = SceneTransformResolutionState::Unvisited;
};

struct SceneTransformSnapshotBuildScratch final
{
	struct IndexEntry final
	{
		uint32 Index = 0;
		uint64 Generation = 0;
	};

	std::vector<ObjectHandle> Objects;
	std::vector<SceneTransformSource> Sources;
	std::unordered_map<ObjectHandle, IndexEntry, ObjectHandleHash> Indices;
	std::vector<uint32> Chain;
	uint64 IndexGeneration = 0;
};

class ENGINE_API SceneTransformSnapshot final
{
  public:
	[[nodiscard]] static SceneTransformSnapshot Build(const Scene::ReadAccess &Access);
	static void BuildInto(const Scene::ReadAccess &Access, SceneTransformSnapshot &Result, SceneTransformSnapshotBuildScratch &Scratch);

	[[nodiscard]] const glm::mat4 &GetMatrix(ObjectHandle Object) const;
	[[nodiscard]] glm::vec3 GetPosition(ObjectHandle Object) const;
	[[nodiscard]] glm::vec3 GetForward(ObjectHandle Object) const;
	[[nodiscard]] usize Size() const noexcept;

  private:
	struct MatrixEntry final
	{
		glm::mat4 Matrix{1.0f};
		uint64 Generation = 0;
	};

	std::unordered_map<ObjectHandle, MatrixEntry, ObjectHandleHash> Matrices;
	uint64 MatrixGeneration = 0;
	usize MatrixCount = 0;
};
} // namespace world
