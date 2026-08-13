#include "EditorViewportController.h"

#include "Source/component/object/CObjectIdentityComponent.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace editor::viewport
{
pipeline::render::PickRequestID EditorViewportController::QueuePick(const float32 NormalizedX, const float32 NormalizedYFromTop,
																	const SelectionOperation Operation)
{
	if (!std::isfinite(NormalizedX) || !std::isfinite(NormalizedYFromTop) || NormalizedX < 0.0f || NormalizedX > 1.0f ||
		NormalizedYFromTop < 0.0f || NormalizedYFromTop > 1.0f)
	{
		throw std::out_of_range("Viewport pick coordinates must be finite normalized values in the inclusive [0, 1] range");
	}

	const pipeline::render::PickRequestID Request = this->NextRequest;
	++this->NextRequest;
	if (this->NextRequest == 0)
		this->NextRequest = 1;
	if (this->PendingPicks.contains(Request))
		throw std::overflow_error("Viewport pick request identity wrapped while the previous identity is still pending");

	this->PendingPicks.emplace(Request,
							   PendingPick{.NormalizedX = NormalizedX, .NormalizedYFromTop = NormalizedYFromTop, .Operation = Operation});
	this->QueuedPicks.push_back(Request);
	return Request;
}

std::vector<ViewportPickRequest> EditorViewportController::CollectPickRequests(const core::WindowExtent CurrentExtent)
{
	std::vector<ViewportPickRequest> Requests;
	this->CollectPickRequestsInto(CurrentExtent, Requests);
	return Requests;
}

void EditorViewportController::CollectPickRequestsInto(const core::WindowExtent CurrentExtent, std::vector<ViewportPickRequest> &Requests)
{
	Requests.clear();
	if (!CurrentExtent.IsValid())
		return;
	if (Requests.capacity() < this->QueuedPicks.size())
		Requests.reserve(this->QueuedPicks.size());
	while (!this->QueuedPicks.empty())
	{
		const pipeline::render::PickRequestID Request = this->QueuedPicks.front();
		this->QueuedPicks.pop_front();
		const auto Pending = this->PendingPicks.find(Request);
		if (Pending == this->PendingPicks.end() || Pending->second.State != PickState::Queued)
			throw std::logic_error("Viewport pick queue lost its matching queued request state");

		const uint32 X =
			std::min(static_cast<uint32>(Pending->second.NormalizedX * static_cast<float32>(CurrentExtent.Width)), CurrentExtent.Width - 1);
		const uint32 YFromTop = std::min(
			static_cast<uint32>(Pending->second.NormalizedYFromTop * static_cast<float32>(CurrentExtent.Height)), CurrentExtent.Height - 1);
		Requests.push_back({.Request = Request, .X = X, .Y = CurrentExtent.Height - 1 - YFromTop});
		Pending->second.State = PickState::InFlight;
	}
}

void EditorViewportController::ApplyFrame(document::SceneDocument &Document, const EditorViewportFrame &Frame)
{
	for (const pipeline::render::PickRequestID Request : Frame.DeferredPicks)
	{
		const auto Pending = this->PendingPicks.find(Request);
		if (Pending == this->PendingPicks.end() || Pending->second.State != PickState::InFlight)
			throw std::logic_error("Viewport renderer deferred an unknown or non-flight pick request");
		Pending->second.State = PickState::Queued;
		this->QueuedPicks.push_back(Request);
	}

	for (const ViewportPickResult &Result : Frame.CompletedPicks)
	{
		const auto Pending = this->PendingPicks.find(Result.Request);
		if (Pending == this->PendingPicks.end() || Pending->second.State != PickState::InFlight)
			throw std::logic_error("Viewport renderer completed an unknown or non-flight pick request");
		this->ApplySelection(Document, Result, Pending->second.Operation);
		this->PendingPicks.erase(Pending);
	}
}

void EditorViewportController::CancelPendingPicks() noexcept
{
	this->PendingPicks.clear();
	this->QueuedPicks.clear();
}

usize EditorViewportController::GetPendingPickCount() const noexcept
{
	return this->PendingPicks.size();
}

void EditorViewportController::ApplySelection(document::SceneDocument &Document, const ViewportPickResult &Result,
											  const SelectionOperation Operation)
{
	if (!Result.Object.has_value())
	{
		if (Operation == SelectionOperation::Replace)
			Document.GetSelection().Clear();
		return;
	}

	world::Scene &Scene = Document.GetScene();
	if (!Scene.Contains(*Result.Object))
		return;
	const world::ComponentHandle<components::CObjectIdentityComponent> Identity =
		Scene.GetComponent<components::CObjectIdentityComponent>(*Result.Object);
	if (!Identity.IsValid())
		throw std::logic_error("A pickable editor object does not have its required persistent identity component");

	util::UUID PersistentID;
	{
		auto Access = Scene.Read();
		const components::CObjectIdentityComponent &IdentityComponent = Access.Resolve(Identity);
		if (IdentityComponent.IsLocked() || !IdentityComponent.IsEnabled() || !IdentityComponent.IsEditorVisible())
			return;
		PersistentID = IdentityComponent.GetPersistentID();
	}
	switch (Operation)
	{
	case SelectionOperation::Replace:
		Document.GetSelection().SelectOnly(PersistentID);
		break;
	case SelectionOperation::Add:
		Document.GetSelection().Add(PersistentID);
		break;
	case SelectionOperation::Toggle:
		Document.GetSelection().Toggle(PersistentID);
		break;
	default:
		throw std::logic_error("Viewport selection operation is invalid");
	}
}
} // namespace editor::viewport
