#pragma once

#include "Source/types.h"

#include <glm.hpp>

namespace pipeline::render
{
enum class ViewportViewMode : uint8
{
	Lit,
	Unlit,
	Wireframe,
	WorldNormals,
	LinearDepth,
	ObjectID,
	Overdraw,
	Count
};

struct ViewportOverlaySettings final
{
	bool Grid = false;
	bool Bounds = false;
	bool Skeletons = false;
	bool Cameras = false;
	bool Lights = false;
	bool Culling = false;
	bool Selection = true;
	bool RenderStatistics = false;
	bool RenderGraph = false;

	[[nodiscard]] bool operator==(const ViewportOverlaySettings &) const noexcept = default;
};

struct ViewportSettings final
{
	ViewportViewMode ViewMode = ViewportViewMode::Lit;
	ViewportOverlaySettings Overlays;

	[[nodiscard]] bool operator==(const ViewportSettings &) const noexcept = default;
};

struct ViewportPickPixel final
{
	uint32 X = 0;
	uint32 Y = 0;

	[[nodiscard]] bool operator==(const ViewportPickPixel &) const noexcept = default;
};

struct alignas(16) GPUDebugLineRecord final
{
	glm::vec4 StartAndWidth{0.0f, 0.0f, 0.0f, 1.0f};
	glm::vec4 End{0.0f};
	glm::vec4 Color{1.0f};
};

struct TransformGizmoOverlay final
{
	bool Visible = false;
	glm::vec3 Pivot{0.0f};
	glm::mat3 Basis{1.0f};
	float32 WorldScale = 1.0f;
	uint32 Operation = 0;
	uint32 ActiveHandle = 0;
	uint32 CapabilityMask = 7;
};

static_assert(sizeof(GPUDebugLineRecord) == sizeof(glm::vec4) * 3U);
} // namespace pipeline::render
