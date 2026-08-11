#include "EditorViewportRenderer.h"

#include "src/resource/asset/AssetManager.h"
#include "src/scene/Scene.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace editor::viewport
{
EditorViewportRenderer::EditorViewportRenderer(pipeline::device::Device &Device, const pipeline::render::RenderViewID View)
	: View(View), Picker(Device), OwnerThread(std::this_thread::get_id())
{
	if (!View.IsValid())
		throw std::invalid_argument("An editor viewport requires a valid render-view identity");
	this->ReadbacksScratch.reserve(pipeline::render::ViewportPicker::ReadbackSlotCount);
	this->PickPixelsScratch.reserve(pipeline::render::ViewportPicker::ReadbackSlotCount);
	this->FrameScratch.CompletedPicks.reserve(pipeline::render::ViewportPicker::ReadbackSlotCount);
	this->FrameScratch.DeferredPicks.reserve(pipeline::render::ViewportPicker::ReadbackSlotCount);
	this->DurableCompletedPicks.reserve(pipeline::render::ViewportPicker::ReadbackSlotCount);
}

EditorViewportFrame EditorViewportRenderer::Render(pipeline::render::Renderer &Renderer, pipeline::render::RenderPipelineLibrary &Pipelines,
												   world::Scene &Scene, resource::AssetManager &Assets, Camera &Camera,
												   const core::WindowExtent Extent, const std::span<const ViewportPickRequest> PickRequests,
												   const std::span<const world::ObjectHandle> SelectedObjects,
												   std::optional<pipeline::render::TransformGizmoOverlay> GizmoOverlay,
												   const pipeline::render::ViewportSettings Settings)
{
	pipeline::render::SceneRenderSnapshotBuilder::BuildInto(Scene, this->SceneSnapshotScratch, {.RespectEditorVisibility = true},
															this->SceneSnapshotBuildScratch);
	return this->Render(Renderer, Pipelines, this->SceneSnapshotScratch, Assets, Camera, Extent, PickRequests, SelectedObjects,
						std::move(GizmoOverlay), Settings);
}

EditorViewportFrame EditorViewportRenderer::Render(pipeline::render::Renderer &Renderer, pipeline::render::RenderPipelineLibrary &Pipelines,
												   const pipeline::render::SceneRenderSnapshot &Scene, resource::AssetManager &Assets,
												   const Camera &Camera, const core::WindowExtent Extent,
												   const std::span<const ViewportPickRequest> PickRequests,
												   const std::span<const world::ObjectHandle> SelectedObjects,
												   std::optional<pipeline::render::TransformGizmoOverlay> GizmoOverlay,
												   const pipeline::render::ViewportSettings Settings)
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw std::logic_error("Editor viewport rendering must remain on its owner thread");
	if (this->Released)
		throw std::logic_error("A released editor viewport cannot render");
	for (usize Index = 0; Index < PickRequests.size(); ++Index)
	{
		const ViewportPickRequest &Request = PickRequests[Index];
		if (Request.Request == 0)
			throw std::invalid_argument("An editor viewport pick request identity must be non-zero");
		if (this->FindPendingPick(Request.Request) != nullptr ||
			std::ranges::find(PickRequests.begin(), PickRequests.begin() + Index, Request.Request, &ViewportPickRequest::Request) !=
				PickRequests.begin() + Index)
		{
			throw std::invalid_argument("An editor viewport cannot submit a duplicate in-flight pick request");
		}
	}
	if (!Extent.IsValid())
	{
		EditorViewportFrame DeferredFrame = std::move(this->FrameScratch);
		DeferredFrame.Output = {};
		DeferredFrame.CompletedPicks.clear();
		DeferredFrame.DeferredPicks.clear();
		for (const ViewportPickRequest &Request : PickRequests)
			DeferredFrame.DeferredPicks.push_back(Request.Request);
		return DeferredFrame;
	}

	EditorViewportFrame Frame = std::move(this->FrameScratch);
	Frame.CompletedPicks.clear();
	Frame.DeferredPicks.clear();
	try
	{
		this->PollCompletedPicksInto(this->DurableCompletedPicks);
		const usize AvailableRequests = this->Picker.GetAvailableRequestCount();
		this->PickPixelsScratch.clear();
		this->PickPixelsScratch.reserve(std::min(PickRequests.size(), AvailableRequests));
		for (usize Index = 0; Index < PickRequests.size(); ++Index)
		{
			const ViewportPickRequest &Request = PickRequests[Index];
			if (Index >= AvailableRequests)
			{
				Frame.DeferredPicks.push_back(Request.Request);
				continue;
			}
			this->PickPixelsScratch.push_back({.X = Request.X, .Y = Request.Y});
		}
		Renderer.Render(Scene, Assets, Camera, this->View, SelectedObjects, std::move(GizmoOverlay), Settings, this->PickPixelsScratch);
		Frame.Output = Renderer.RenderView(Pipelines.GetPipelineSet(), Camera,
										   pipeline::render::RenderViewDescriptor{.View = this->View, .Extent = Extent});
		if (!Frame.Output.IsValid())
			throw std::runtime_error("The editor viewport renderer produced an invalid render-view output");

		Frame.DeferredPicks.reserve(PickRequests.size() - this->PickPixelsScratch.size());
		for (usize Index = 0; Index < this->PickPixelsScratch.size(); ++Index)
		{
			const ViewportPickRequest &Request = PickRequests[Index];
			PendingPick *Pending = this->FindAvailablePendingPick();
			if (Pending == nullptr)
				throw std::logic_error("Viewport picker accepted a request without a pending-pick slot");
			const bool Accepted = this->Picker.TryRequest(Frame.Output.ObjectID.Texture, Frame.Output.ObjectID.Extent, Request.X, Request.Y,
														  Request.Request, Frame.Output.FrameNumber);
			if (!Accepted)
				throw std::logic_error("Viewport picker capacity changed while publishing a completed render frame");
			*Pending = {
				.Request = Request.Request, .SourceFrame = Frame.Output.FrameNumber, .Table = Frame.Output.PickTable, .Active = true};
		}
		Frame.CompletedPicks = std::move(this->DurableCompletedPicks);
		this->DurableCompletedPicks.clear();
		return Frame;
	}
	catch (...)
	{
		this->FrameScratch = std::move(Frame);
		throw;
	}
}

void EditorViewportRenderer::Release(pipeline::render::Renderer &Renderer)
{
	if (this->Released)
		return;
	if (std::this_thread::get_id() != this->OwnerThread)
		throw std::logic_error("Editor viewport release must run on its owner thread");
	this->Released = true;
	this->Picker.CancelAll();
	for (PendingPick &Pending : this->PendingPicks)
		Pending = {};
	if (this->View.IsValid())
		Renderer.ReleaseView(this->View);
	// Do not retain frame outputs after their render-view generation has been
	// released.  The scratch packet is only a capacity cache between frames.
	this->FrameScratch.Output = {};
	this->FrameScratch.CompletedPicks.clear();
	this->FrameScratch.DeferredPicks.clear();
	this->DurableCompletedPicks.clear();
}

void EditorViewportRenderer::RecycleFrame(EditorViewportFrame Frame) noexcept
{
	this->FrameScratch = std::move(Frame);
}

pipeline::render::RenderViewID EditorViewportRenderer::GetView() const noexcept
{
	return this->View;
}

usize EditorViewportRenderer::GetPendingPickCount() const noexcept
{
	return static_cast<usize>(std::ranges::count_if(this->PendingPicks, [](const PendingPick &Pending) { return Pending.Active; }));
}

std::vector<ViewportPickResult> EditorViewportRenderer::PollCompletedPicks()
{
	std::vector<ViewportPickResult> Results;
	this->PollCompletedPicksInto(Results);
	return Results;
}

void EditorViewportRenderer::PollCompletedPicksInto(std::vector<ViewportPickResult> &Results)
{
	this->Picker.PollInto(this->ReadbacksScratch);
	Results.reserve(Results.size() + this->ReadbacksScratch.size());
	for (const pipeline::render::PickReadbackResult &Readback : this->ReadbacksScratch)
	{
		PendingPick *Pending = this->FindPendingPick(Readback.Request);
		if (Pending == nullptr || Pending->Table == nullptr || Pending->SourceFrame != Readback.SourceFrame ||
			Pending->Table->GetFrameNumber() != Readback.SourceFrame)
		{
			throw std::logic_error("An editor viewport pick completed without its matching frame-resolution table");
		}
		Results.push_back(
			{.Request = Readback.Request, .Object = Pending->Table->Resolve(Readback.Pick), .SourceFrame = Readback.SourceFrame});
		*Pending = {};
	}
}

EditorViewportRenderer::PendingPick *EditorViewportRenderer::FindPendingPick(const pipeline::render::PickRequestID Request) noexcept
{
	const auto Found = std::ranges::find_if(this->PendingPicks,
											[Request](const PendingPick &Pending) { return Pending.Active && Pending.Request == Request; });
	return Found == this->PendingPicks.end() ? nullptr : &*Found;
}

const EditorViewportRenderer::PendingPick *EditorViewportRenderer::FindPendingPick(
	const pipeline::render::PickRequestID Request) const noexcept
{
	const auto Found = std::ranges::find_if(this->PendingPicks,
											[Request](const PendingPick &Pending) { return Pending.Active && Pending.Request == Request; });
	return Found == this->PendingPicks.end() ? nullptr : &*Found;
}

EditorViewportRenderer::PendingPick *EditorViewportRenderer::FindAvailablePendingPick() noexcept
{
	const auto Found = std::ranges::find_if(this->PendingPicks, [](const PendingPick &Pending) { return !Pending.Active; });
	return Found == this->PendingPicks.end() ? nullptr : &*Found;
}
} // namespace editor::viewport
