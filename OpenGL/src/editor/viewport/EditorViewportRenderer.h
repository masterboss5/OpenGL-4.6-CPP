#pragma once

#include "src/pipeline/render/RenderPipelineLibrary.h"
#include "src/pipeline/render/Renderer.h"
#include "src/pipeline/render/SceneRenderSnapshot.h"
#include "src/pipeline/render/ViewportPicker.h"
#include "src/scene/Camera.h"
#include "src/scene/SceneHandles.h"
#include "src/types.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace resource
{
class AssetManager;
}

namespace world
{
class Scene;
}

namespace editor::viewport
{
struct ViewportPickRequest final
{
	pipeline::render::PickRequestID Request = 0;
	uint32 X = 0;
	uint32 Y = 0;
};

struct ViewportPickResult final
{
	pipeline::render::PickRequestID Request = 0;
	std::optional<world::ObjectHandle> Object;
	uint64 SourceFrame = 0;
};

struct EditorViewportFrame final
{
	pipeline::render::RenderViewOutput Output;
	std::vector<ViewportPickResult> CompletedPicks;
	std::vector<pipeline::render::PickRequestID> DeferredPicks;
};

class EditorViewportRenderer final
{
  public:
	EditorViewportRenderer(pipeline::device::Device &Device, pipeline::render::RenderViewID View);
	~EditorViewportRenderer() = default;

	EditorViewportRenderer(const EditorViewportRenderer &) = delete;
	EditorViewportRenderer &operator=(const EditorViewportRenderer &) = delete;
	EditorViewportRenderer(EditorViewportRenderer &&) = delete;
	EditorViewportRenderer &operator=(EditorViewportRenderer &&) = delete;

	[[nodiscard]] EditorViewportFrame Render(pipeline::render::Renderer &Renderer, pipeline::render::RenderPipelineLibrary &Pipelines,
											 world::Scene &Scene, resource::AssetManager &Assets, Camera &Camera, core::WindowExtent Extent,
											 std::span<const ViewportPickRequest> PickRequests,
											 std::span<const world::ObjectHandle> SelectedObjects,
											 std::optional<pipeline::render::TransformGizmoOverlay> GizmoOverlay = std::nullopt,
											 pipeline::render::ViewportSettings Settings = {});
	[[nodiscard]] EditorViewportFrame Render(pipeline::render::Renderer &Renderer, pipeline::render::RenderPipelineLibrary &Pipelines,
											 const pipeline::render::SceneRenderSnapshot &Scene, resource::AssetManager &Assets,
											 const Camera &Camera, core::WindowExtent Extent,
											 std::span<const ViewportPickRequest> PickRequests,
											 std::span<const world::ObjectHandle> SelectedObjects,
											 std::optional<pipeline::render::TransformGizmoOverlay> GizmoOverlay = std::nullopt,
											 pipeline::render::ViewportSettings Settings = {});
	void Release(pipeline::render::Renderer &Renderer);
	void RecycleFrame(EditorViewportFrame Frame) noexcept;

	[[nodiscard]] pipeline::render::RenderViewID GetView() const noexcept;
	[[nodiscard]] usize GetPendingPickCount() const noexcept;

  private:
	struct PendingPick final
	{
		pipeline::render::PickRequestID Request = 0;
		uint64 SourceFrame = 0;
		std::shared_ptr<const pipeline::render::FramePickTable> Table;
		bool Active = false;
	};

	[[nodiscard]] std::vector<ViewportPickResult> PollCompletedPicks();
	void PollCompletedPicksInto(std::vector<ViewportPickResult> &Results);
	[[nodiscard]] PendingPick *FindPendingPick(pipeline::render::PickRequestID Request) noexcept;
	[[nodiscard]] const PendingPick *FindPendingPick(pipeline::render::PickRequestID Request) const noexcept;
	[[nodiscard]] PendingPick *FindAvailablePendingPick() noexcept;

	pipeline::render::RenderViewID View;
	pipeline::render::ViewportPicker Picker;
	pipeline::render::SceneRenderSnapshot SceneSnapshotScratch;
	pipeline::render::SceneRenderSnapshotBuildScratch SceneSnapshotBuildScratch;
	EditorViewportFrame FrameScratch;
	std::vector<pipeline::render::PickReadbackResult> ReadbacksScratch;
	std::vector<pipeline::render::ViewportPickPixel> PickPixelsScratch;
	std::vector<ViewportPickResult> DurableCompletedPicks;
	std::array<PendingPick, pipeline::render::ViewportPicker::ReadbackSlotCount> PendingPicks{};
	std::thread::id OwnerThread;
	bool Released = false;
};

// EditorViewport is the plan-facing name for the existing renderer-owned
// viewport boundary.
using EditorViewport = EditorViewportRenderer;
} // namespace editor::viewport
