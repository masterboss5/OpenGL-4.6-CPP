#pragma once

#include "Source/core/window/WindowTypes.h"
#include "Source/editor/commands/TransformEditCommand.h"
#include "Source/editor/document/SceneDocument.h"
#include "Source/scene/Camera.h"
#include "Source/scene/SceneTransformSnapshot.h"
#include "Source/types.h"

#include <glm.hpp>
#include <span>
#include <vector>

namespace editor::viewport
{
enum class TransformGizmoOperation : uint8
{
	Translate,
	Rotate,
	Scale,
	Universal,
	Select,
	Count
};

enum class TransformGizmoSpace : uint8
{
	World,
	Local,
	Count
};

enum class TransformGizmoPivot : uint8
{
	ActiveObject,
	MedianPoint,
	IndividualOrigins,
	BoundingBoxCenter,
	WorldOrigin,
	Count
};

enum class TransformGizmoHandle : uint8
{
	None,
	AxisX,
	AxisY,
	AxisZ,
	PlaneXY,
	PlaneYZ,
	PlaneZX,
	Screen,
	Uniform,
	RotateAxisX,
	RotateAxisY,
	RotateAxisZ,
	RotateScreen,
	ScaleAxisX,
	ScaleAxisY,
	ScaleAxisZ,
	ScalePlaneXY,
	ScalePlaneYZ,
	ScalePlaneZX,
	ScaleUniform
};

struct TransformGizmoSnapSettings final
{
	bool Enabled = false;
	float32 Translation = 1.0f;
	float32 RotationDegrees = 15.0f;
	float32 Scale = 0.1f;
};

struct TransformGizmoVisualState final
{
	bool Visible = false;
	bool Dragging = false;
	bool AllowTranslation = true;
	bool AllowRotation = true;
	bool AllowScale = true;
	glm::vec3 Pivot{0.0f};
	glm::mat3 Basis{1.0f};
	float32 WorldScale = 1.0f;
	TransformGizmoOperation Operation = TransformGizmoOperation::Translate;
	TransformGizmoHandle ActiveHandle = TransformGizmoHandle::None;
};

class TransformGizmoController final
{
  public:
	void SetOperation(TransformGizmoOperation Operation);
	void SetSpace(TransformGizmoSpace Space);
	void SetPivot(TransformGizmoPivot Pivot);
	void SetSnapSettings(const TransformGizmoSnapSettings &Settings);

	[[nodiscard]] bool BeginDrag(document::SceneDocument &Document, const Camera &Camera, core::WindowExtent Extent, float32 NormalizedX,
								 float32 NormalizedYFromTop, TransformGizmoHandle Handle);
	[[nodiscard]] bool UpdateDrag(const Camera &Camera, core::WindowExtent Extent, float32 NormalizedX, float32 NormalizedYFromTop);
	void CommitDrag();
	void CancelDrag();

	[[nodiscard]] TransformGizmoVisualState BuildVisualState(const document::SceneDocument &Document, const Camera &Camera) const;
	[[nodiscard]] TransformGizmoHandle HitTest(const document::SceneDocument &Document, const Camera &Camera, core::WindowExtent Extent,
											   float32 NormalizedX, float32 NormalizedYFromTop, float32 TolerancePixels = 9.0f) const;
	[[nodiscard]] bool IsDragging() const noexcept;
	[[nodiscard]] TransformGizmoOperation GetOperation() const noexcept;
	[[nodiscard]] TransformGizmoSpace GetSpace() const noexcept;
	[[nodiscard]] TransformGizmoPivot GetPivot() const noexcept;
	[[nodiscard]] const TransformGizmoSnapSettings &GetSnapSettings() const noexcept;

  private:
	struct Ray final
	{
		glm::vec3 Origin{0.0f};
		glm::vec3 Direction{0.0f, 0.0f, -1.0f};
	};

	struct DragTarget final
	{
		world::ObjectHandle Object;
		commands::TransformState Initial;
		glm::mat4 InitialWorld{1.0f};
		glm::mat4 ParentWorldInverse{1.0f};
		glm::vec3 WorldBoundsMinimum{0.0f};
		glm::vec3 WorldBoundsMaximum{0.0f};
	};

	[[nodiscard]] static Ray BuildRay(const Camera &Camera, core::WindowExtent Extent, float32 NormalizedX, float32 NormalizedYFromTop);
	[[nodiscard]] static bool IntersectPlane(const Ray &Ray, const glm::vec3 &Point, const glm::vec3 &Normal, glm::vec3 &Intersection);
	[[nodiscard]] static float32 ClosestAxisParameter(const Ray &Ray, const glm::vec3 &Origin, const glm::vec3 &Axis,
													  const glm::vec3 &ViewDirection);
	[[nodiscard]] static float32 Snap(float32 Value, float32 Increment);
	[[nodiscard]] static glm::vec3 AxisForHandle(TransformGizmoHandle Handle, const glm::mat3 &Basis);
	[[nodiscard]] static glm::vec3 PlaneNormalForHandle(TransformGizmoHandle Handle, const glm::mat3 &Basis, const Camera &Camera);
	[[nodiscard]] static commands::TransformState ResolveLocalTransform(const DragTarget &Target, const glm::mat4 &DesiredWorld);
	[[nodiscard]] static instance::InstanceTransformCapabilities ResolveSharedCapabilities(const document::SceneDocument &Document,
																						   const world::Scene::ReadAccess &Access,
																						   std::span<const world::ObjectHandle> Objects);
	[[nodiscard]] glm::vec3 CalculatePivot(const document::SceneDocument &Document, const std::vector<DragTarget> &Targets) const;
	[[nodiscard]] glm::mat3 CalculateBasis(const std::vector<DragTarget> &Targets) const;
	void ApplyTranslation(const Ray &Ray);
	void ApplyRotation(const Ray &Ray);
	void ApplyScale(const Ray &Ray, const Camera &Camera);
	void Publish(std::span<const commands::TransformEditTarget> Targets);

	TransformGizmoOperation Operation = TransformGizmoOperation::Translate;
	TransformGizmoOperation DragOperation = TransformGizmoOperation::Translate;
	TransformGizmoSpace Space = TransformGizmoSpace::World;
	TransformGizmoPivot PivotMode = TransformGizmoPivot::ActiveObject;
	TransformGizmoSnapSettings SnapSettings;
	document::SceneDocument *ActiveDocument = nullptr;
	std::vector<DragTarget> DragTargets;
	std::vector<commands::TransformEditTarget> TransformEditTargetsScratch;
	TransformGizmoHandle ActiveHandle = TransformGizmoHandle::None;
	glm::vec3 DragPivot{0.0f};
	glm::mat3 DragBasis{1.0f};
	glm::vec3 ConstraintAxis{0.0f};
	glm::vec3 ConstraintPlaneNormal{0.0f};
	glm::vec3 StartPoint{0.0f};
	float32 StartAxisParameter = 0.0f;
	float32 WorldScale = 1.0f;
	bool HasPublishedEdit = false;
	mutable std::vector<world::ObjectHandle> SelectedScratch;
	mutable std::vector<DragTarget> VisualTargetsScratch;
	mutable world::SceneTransformSnapshot TransformSnapshotScratch;
	mutable world::SceneTransformSnapshotBuildScratch TransformSnapshotBuildScratch;
};

// TransformGizmo is the plan-facing name for the existing transaction-aware
// gizmo controller.
using TransformGizmo = TransformGizmoController;
} // namespace editor::viewport
