#pragma once

#include "EditorViewportRenderer.h"
#include "Source/core/window/WindowTypes.h"
#include "Source/editor/document/SceneDocument.h"
#include "Source/types.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace editor::viewport
{
enum class SelectionOperation : uint8
{
	Replace,
	Add,
	Toggle
};

class EditorViewportController final
{
  public:
	[[nodiscard]] pipeline::render::PickRequestID QueuePick(float32 NormalizedX, float32 NormalizedYFromTop, SelectionOperation Operation);
	void CollectPickRequestsInto(core::WindowExtent CurrentExtent, std::vector<ViewportPickRequest> &Requests);
	[[nodiscard]] std::vector<ViewportPickRequest> CollectPickRequests(core::WindowExtent CurrentExtent);
	void ApplyFrame(document::SceneDocument &Document, const EditorViewportFrame &Frame);
	void CancelPendingPicks() noexcept;

	[[nodiscard]] usize GetPendingPickCount() const noexcept;

  private:
	enum class PickState : uint8
	{
		Queued,
		InFlight
	};

	struct PendingPick final
	{
		float32 NormalizedX = 0.0f;
		float32 NormalizedYFromTop = 0.0f;
		SelectionOperation Operation = SelectionOperation::Replace;
		PickState State = PickState::Queued;
	};

	void ApplySelection(document::SceneDocument &Document, const ViewportPickResult &Result, SelectionOperation Operation);

	pipeline::render::PickRequestID NextRequest = 1;
	std::unordered_map<pipeline::render::PickRequestID, PendingPick> PendingPicks;
	std::deque<pipeline::render::PickRequestID> QueuedPicks;
};
} // namespace editor::viewport
