#pragma once

#include "Source/pipeline/graph/RenderGraph.h"
#include "Source/pipeline/device/Device.h"
#include "Source/pipeline/render/PickTable.h"
#include "Source/types.h"

#include <GL/glew.h>
#include <array>
#include <optional>
#include <vector>

namespace pipeline::render
{
using PickRequestID = uint64;

struct PickReadbackResult final
{
	PickRequestID Request = 0;
	PickID Pick = BackgroundPickID;
	uint64 SourceFrame = 0;
};

class ENGINE_API ViewportPicker final
{
  public:
	static constexpr usize ReadbackSlotCount = 3;

	explicit ViewportPicker(pipeline::device::Device &Device);
	~ViewportPicker();

	ViewportPicker(const ViewportPicker &) = delete;
	ViewportPicker &operator=(const ViewportPicker &) = delete;
	ViewportPicker(ViewportPicker &&) = delete;
	ViewportPicker &operator=(ViewportPicker &&) = delete;

	[[nodiscard]] bool TryRequest(GLuint ObjectIDTexture, pipeline::graph::Extent2D Extent, uint32 X, uint32 Y, PickRequestID Request,
								  uint64 SourceFrame);
	void PollInto(std::vector<PickReadbackResult> &Results);
	[[nodiscard]] std::vector<PickReadbackResult> Poll();
	void CancelAll() noexcept;
	[[nodiscard]] usize GetPendingCount() const noexcept;
	[[nodiscard]] usize GetAvailableRequestCount() const noexcept;

  private:
	struct ReadbackSlot final
	{
		GLuint Buffer = 0;
		PickID *Mapped = nullptr;
		GLsync Fence = nullptr;
		PickRequestID Request = 0;
		uint64 SourceFrame = 0;
		bool Pending = false;
	};

	[[nodiscard]] std::optional<PickReadbackResult> PollSlot(ReadbackSlot &Slot);
	void ResetSlot(ReadbackSlot &Slot) noexcept;

	pipeline::device::DeviceHandle Device;
	std::array<ReadbackSlot, ReadbackSlotCount> Slots;
	usize NextSlot = 0;
	usize PendingCount = 0;
};

// ObjectPicker is the plan-facing name; the viewport picker remains the
// canonical implementation and owns the fenced PBO ring.
using ObjectPicker = ViewportPicker;
} // namespace pipeline::render
