#include "TransformGizmoController.h"

#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectHierarchyComponent.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/scene/SceneTransformSnapshot.h"
#include "src/scene/TransformMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <gtc/constants.hpp>
#include <gtc/matrix_transform.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace editor::viewport
{
namespace
{
constexpr float32 MinimumMagnitude = 1.0e-6f;
// Keep interactive scale comfortably above the affine decomposition boundary
// (1e-4) so floating-point rounding cannot turn a clamped drag into a
// singular transform.
constexpr float32 MinimumInteractiveScaleFactor = 1.0e-3f;

[[nodiscard]] bool IsFinite(const glm::vec3 &Value)
{
	return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

[[nodiscard]] std::pair<glm::vec3, glm::vec3> CalculateWorldBounds(const world::Scene::ReadAccess &Access, const world::ObjectHandle Object,
																   const glm::mat4 &World)
{
	const glm::vec3 Origin(World[3]);
	const auto Mesh = Access.GetComponent<components::CObjectMeshComponent>(Object);
	if (!Mesh.IsValid())
		return {Origin, Origin};
	const resource::AssetPtr<resource::ModelAsset> Model = Access.Resolve(Mesh).GetModel().TryPin();
	if (!Model)
		return {Origin, Origin};
	const resource::Bounds &Bounds = Model->GetBounds();
	if (!Bounds.IsValid())
		return {Origin, Origin};

	glm::vec3 Minimum(std::numeric_limits<float32>::max());
	glm::vec3 Maximum(std::numeric_limits<float32>::lowest());
	for (uint8 Corner = 0; Corner < 8; ++Corner)
	{
		const glm::vec3 Local((Corner & 1U) != 0 ? Bounds.Maximum.x : Bounds.Minimum.x,
							  (Corner & 2U) != 0 ? Bounds.Maximum.y : Bounds.Minimum.y,
							  (Corner & 4U) != 0 ? Bounds.Maximum.z : Bounds.Minimum.z);
		const glm::vec3 Point = glm::vec3(World * glm::vec4(Local, 1.0f));
		Minimum = glm::min(Minimum, Point);
		Maximum = glm::max(Maximum, Point);
	}
	return {Minimum, Maximum};
}

[[nodiscard]] bool IsAxisHandle(const TransformGizmoHandle Handle) noexcept
{
	return Handle == TransformGizmoHandle::AxisX || Handle == TransformGizmoHandle::AxisY || Handle == TransformGizmoHandle::AxisZ ||
		   Handle == TransformGizmoHandle::RotateAxisX || Handle == TransformGizmoHandle::RotateAxisY ||
		   Handle == TransformGizmoHandle::RotateAxisZ || Handle == TransformGizmoHandle::ScaleAxisX ||
		   Handle == TransformGizmoHandle::ScaleAxisY || Handle == TransformGizmoHandle::ScaleAxisZ;
}

[[nodiscard]] bool IsPlaneHandle(const TransformGizmoHandle Handle) noexcept
{
	return Handle == TransformGizmoHandle::PlaneXY || Handle == TransformGizmoHandle::PlaneYZ || Handle == TransformGizmoHandle::PlaneZX ||
		   Handle == TransformGizmoHandle::ScalePlaneXY || Handle == TransformGizmoHandle::ScalePlaneYZ ||
		   Handle == TransformGizmoHandle::ScalePlaneZX;
}

[[nodiscard]] TransformGizmoOperation OperationForHandle(const TransformGizmoHandle Handle)
{
	switch (Handle)
	{
	case TransformGizmoHandle::RotateAxisX:
	case TransformGizmoHandle::RotateAxisY:
	case TransformGizmoHandle::RotateAxisZ:
	case TransformGizmoHandle::RotateScreen:
		return TransformGizmoOperation::Rotate;
	case TransformGizmoHandle::ScaleAxisX:
	case TransformGizmoHandle::ScaleAxisY:
	case TransformGizmoHandle::ScaleAxisZ:
	case TransformGizmoHandle::ScalePlaneXY:
	case TransformGizmoHandle::ScalePlaneYZ:
	case TransformGizmoHandle::ScalePlaneZX:
	case TransformGizmoHandle::ScaleUniform:
	case TransformGizmoHandle::Uniform:
		return TransformGizmoOperation::Scale;
	case TransformGizmoHandle::AxisX:
	case TransformGizmoHandle::AxisY:
	case TransformGizmoHandle::AxisZ:
	case TransformGizmoHandle::PlaneXY:
	case TransformGizmoHandle::PlaneYZ:
	case TransformGizmoHandle::PlaneZX:
	case TransformGizmoHandle::Screen:
		return TransformGizmoOperation::Translate;
	default:
		throw std::invalid_argument("Transform-gizmo handle does not encode an operation");
	}
}

[[nodiscard]] glm::vec3 EnabledScaleAxes(const TransformGizmoHandle Handle)
{
	switch (Handle)
	{
	case TransformGizmoHandle::AxisX:
	case TransformGizmoHandle::ScaleAxisX:
		return {1.0f, 0.0f, 0.0f};
	case TransformGizmoHandle::AxisY:
	case TransformGizmoHandle::ScaleAxisY:
		return {0.0f, 1.0f, 0.0f};
	case TransformGizmoHandle::AxisZ:
	case TransformGizmoHandle::ScaleAxisZ:
		return {0.0f, 0.0f, 1.0f};
	case TransformGizmoHandle::PlaneXY:
	case TransformGizmoHandle::ScalePlaneXY:
		return {1.0f, 1.0f, 0.0f};
	case TransformGizmoHandle::PlaneYZ:
	case TransformGizmoHandle::ScalePlaneYZ:
		return {0.0f, 1.0f, 1.0f};
	case TransformGizmoHandle::PlaneZX:
	case TransformGizmoHandle::ScalePlaneZX:
		return {1.0f, 0.0f, 1.0f};
	case TransformGizmoHandle::Screen:
	case TransformGizmoHandle::Uniform:
	case TransformGizmoHandle::ScaleUniform:
		return {1.0f, 1.0f, 1.0f};
	default:
		return {0.0f, 0.0f, 0.0f};
	}
}

[[nodiscard]] bool ProjectToPixels(const glm::vec3 &Point, const glm::mat4 &ViewProjection, const core::WindowExtent Extent,
								   glm::vec2 &Pixels)
{
	const glm::vec4 Clip = ViewProjection * glm::vec4(Point, 1.0f);
	if (Clip.w <= 0.0f)
		return false;
	const glm::vec2 NDC = glm::vec2(Clip) / Clip.w;
	Pixels = {(NDC.x * 0.5f + 0.5f) * static_cast<float32>(Extent.Width), (0.5f - NDC.y * 0.5f) * static_cast<float32>(Extent.Height)};
	return std::isfinite(Pixels.x) && std::isfinite(Pixels.y);
}

[[nodiscard]] float32 PointSegmentDistance(const glm::vec2 &Point, const glm::vec2 &Start, const glm::vec2 &End)
{
	const glm::vec2 Segment = End - Start;
	const float32 LengthSquared = glm::dot(Segment, Segment);
	if (LengthSquared <= MinimumMagnitude)
		return glm::distance(Point, Start);
	const float32 Alpha = std::clamp(glm::dot(Point - Start, Segment) / LengthSquared, 0.0f, 1.0f);
	return glm::distance(Point, Start + Segment * Alpha);
}

[[nodiscard]] bool PointInConvexQuad(const glm::vec2 &Point, const std::array<glm::vec2, 4> &Corners)
{
	float32 DoubleArea = 0.0f;
	for (uint32 Edge = 0; Edge < Corners.size(); ++Edge)
	{
		const glm::vec2 A = Corners[Edge];
		const glm::vec2 B = Corners[(Edge + 1U) % Corners.size()];
		DoubleArea += A.x * B.y - B.x * A.y;
	}
	if (std::abs(DoubleArea) <= 1.0f)
		return false;

	float32 PreviousCross = 0.0f;
	for (uint32 Edge = 0; Edge < Corners.size(); ++Edge)
	{
		const glm::vec2 A = Corners[Edge];
		const glm::vec2 B = Corners[(Edge + 1U) % Corners.size()];
		const glm::vec2 Segment = B - A;
		const glm::vec2 Delta = Point - A;
		const float32 Cross = Segment.x * Delta.y - Segment.y * Delta.x;
		if (std::abs(Cross) <= MinimumMagnitude)
			continue;
		if (PreviousCross != 0.0f && std::signbit(Cross) != std::signbit(PreviousCross))
			return false;
		PreviousCross = Cross;
	}
	return true;
}
} // namespace

void TransformGizmoController::SetOperation(const TransformGizmoOperation Operation)
{
	if (this->IsDragging())
		throw std::logic_error("Cannot change transform-gizmo operation during a drag");
	if (Operation >= TransformGizmoOperation::Count)
		throw std::invalid_argument("Transform-gizmo operation is invalid");
	this->Operation = Operation;
}

void TransformGizmoController::SetSpace(const TransformGizmoSpace Space)
{
	if (this->IsDragging())
		throw std::logic_error("Cannot change transform-gizmo space during a drag");
	if (Space >= TransformGizmoSpace::Count)
		throw std::invalid_argument("Transform-gizmo space is invalid");
	this->Space = Space;
}

void TransformGizmoController::SetPivot(const TransformGizmoPivot Pivot)
{
	if (this->IsDragging())
		throw std::logic_error("Cannot change transform-gizmo pivot during a drag");
	if (Pivot >= TransformGizmoPivot::Count)
		throw std::invalid_argument("Transform-gizmo pivot is invalid");
	this->PivotMode = Pivot;
}

void TransformGizmoController::SetSnapSettings(const TransformGizmoSnapSettings &Settings)
{
	if (!std::isfinite(Settings.Translation) || Settings.Translation <= 0.0f || !std::isfinite(Settings.RotationDegrees) ||
		Settings.RotationDegrees <= 0.0f || !std::isfinite(Settings.Scale) || Settings.Scale <= 0.0f)
	{
		throw std::invalid_argument("Transform-gizmo snap increments must be finite and positive");
	}
	this->SnapSettings = Settings;
}

bool TransformGizmoController::BeginDrag(document::SceneDocument &Document, const Camera &Camera, const core::WindowExtent Extent,
										 const float32 NormalizedX, const float32 NormalizedYFromTop, const TransformGizmoHandle Handle)
{
	if (this->IsDragging())
		throw std::logic_error("A transform-gizmo drag is already active");
	if (this->Operation == TransformGizmoOperation::Select)
		return false;
	if (Handle == TransformGizmoHandle::None)
		throw std::invalid_argument("A transform-gizmo drag requires an interactive handle");
	if (this->Operation != TransformGizmoOperation::Universal &&
		((this->Operation == TransformGizmoOperation::Rotate && (IsPlaneHandle(Handle) || Handle == TransformGizmoHandle::Uniform)) ||
		 (this->Operation == TransformGizmoOperation::Translate && Handle == TransformGizmoHandle::Uniform) ||
		 (this->Operation != TransformGizmoOperation::Rotate &&
		  (Handle == TransformGizmoHandle::RotateAxisX || Handle == TransformGizmoHandle::RotateAxisY ||
		   Handle == TransformGizmoHandle::RotateAxisZ || Handle == TransformGizmoHandle::RotateScreen)) ||
		 (this->Operation != TransformGizmoOperation::Scale &&
		  (Handle == TransformGizmoHandle::ScaleAxisX || Handle == TransformGizmoHandle::ScaleAxisY ||
		   Handle == TransformGizmoHandle::ScaleAxisZ || Handle == TransformGizmoHandle::ScalePlaneXY ||
		   Handle == TransformGizmoHandle::ScalePlaneYZ || Handle == TransformGizmoHandle::ScalePlaneZX ||
		   Handle == TransformGizmoHandle::ScaleUniform))))
	{
		throw std::invalid_argument("The selected handle is incompatible with the transform-gizmo operation");
	}
	this->DragOperation = this->Operation == TransformGizmoOperation::Universal ? OperationForHandle(Handle) : this->Operation;

	this->SelectedScratch.clear();
	Document.GetSelection().ResolveInto(Document.GetScene(), this->SelectedScratch);
	const std::vector<world::ObjectHandle> &Selected = this->SelectedScratch;
	if (Selected.empty())
		return false;

	this->DragTargets.clear();
	this->DragTargets.reserve(Selected.size());
	const auto Access = Document.GetScene().Read();
	world::SceneTransformSnapshot::BuildInto(Access, this->TransformSnapshotScratch, this->TransformSnapshotBuildScratch);
	const world::SceneTransformSnapshot &WorldTransforms = this->TransformSnapshotScratch;
	for (const world::ObjectHandle Object : Selected)
	{
		const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Object);
		if (Identity.IsValid())
		{
			const components::CObjectIdentityComponent &IdentityComponent = Access.Resolve(Identity);
			if (IdentityComponent.IsLocked() || !IdentityComponent.IsEnabled() || !IdentityComponent.IsEditorVisible())
				throw world::SceneException("Selected gizmo target is locked, disabled, or hidden");
		}
		const world::ComponentHandle<components::CObjectTransformComponent> Transform =
			Access.GetComponent<components::CObjectTransformComponent>(Object);
		if (!Transform.IsValid())
			throw world::SceneException("Selected gizmo target has no CObjectTransformComponent");
		const components::CObjectTransformComponent &Component = Access.Resolve(Transform);
		glm::mat4 ParentWorld(1.0f);
		const auto Hierarchy = Access.GetComponent<components::CObjectHierarchyComponent>(Object);
		if (Hierarchy.IsValid())
		{
			const world::ObjectHandle Parent = Access.Resolve(Hierarchy).GetParent();
			if (Parent.IsValid())
				ParentWorld = WorldTransforms.GetMatrix(Parent);
		}
		if (std::abs(glm::determinant(glm::mat3(ParentWorld))) <= MinimumMagnitude)
			throw world::SceneException("Selected gizmo target has a non-invertible parent transform");
		const glm::mat4 InitialWorld = WorldTransforms.GetMatrix(Object);
		const auto [WorldBoundsMinimum, WorldBoundsMaximum] = CalculateWorldBounds(Access, Object, InitialWorld);
		this->DragTargets.push_back(
			{.Object = Object,
			 .Initial = {.Position = Component.GetPosition(), .Rotation = Component.GetRotation(), .Scale = Component.GetScale()},
			 .InitialWorld = InitialWorld,
			 .ParentWorldInverse = glm::inverse(ParentWorld),
			 .WorldBoundsMinimum = WorldBoundsMinimum,
			 .WorldBoundsMaximum = WorldBoundsMaximum});
	}

	this->ActiveHandle = Handle;
	this->DragBasis = this->CalculateBasis(this->DragTargets);
	this->DragPivot = this->CalculatePivot(Document, this->DragTargets);
	this->WorldScale = std::max(glm::distance(Camera.Position, this->DragPivot) * 0.15f, 0.1f);
	this->ConstraintAxis = AxisForHandle(Handle, this->DragBasis);
	this->ConstraintPlaneNormal = PlaneNormalForHandle(Handle, this->DragBasis, Camera);
	const Ray CursorRay = BuildRay(Camera, Extent, NormalizedX, NormalizedYFromTop);
	if (IsAxisHandle(Handle) && this->DragOperation != TransformGizmoOperation::Rotate)
	{
		this->StartAxisParameter = ClosestAxisParameter(CursorRay, this->DragPivot, this->ConstraintAxis, Camera.Front);
	}
	else if (!IntersectPlane(CursorRay, this->DragPivot, this->ConstraintPlaneNormal, this->StartPoint))
	{
		this->DragTargets.clear();
		this->ActiveHandle = TransformGizmoHandle::None;
		return false;
	}
	this->HasPublishedEdit = false;
	Document.GetHistory().BeginTransaction("Transform selection");
	this->ActiveDocument = &Document;
	return true;
}

bool TransformGizmoController::UpdateDrag(const Camera &Camera, const core::WindowExtent Extent, const float32 NormalizedX,
										  const float32 NormalizedYFromTop)
{
	if (!this->IsDragging())
		return false;
	const Ray CursorRay = BuildRay(Camera, Extent, NormalizedX, NormalizedYFromTop);
	switch (this->DragOperation)
	{
	case TransformGizmoOperation::Translate:
		this->ApplyTranslation(CursorRay);
		break;
	case TransformGizmoOperation::Rotate:
		this->ApplyRotation(CursorRay);
		break;
	case TransformGizmoOperation::Scale:
		this->ApplyScale(CursorRay, Camera);
		break;
	default:
		throw std::logic_error("Transform-gizmo operation is invalid");
	}
	return true;
}

void TransformGizmoController::CommitDrag()
{
	if (!this->IsDragging())
		return;
	this->ActiveDocument->GetHistory().CommitTransaction();
	this->ActiveDocument = nullptr;
	this->DragTargets.clear();
	this->ActiveHandle = TransformGizmoHandle::None;
	this->HasPublishedEdit = false;
}

void TransformGizmoController::CancelDrag()
{
	if (!this->IsDragging())
		return;
	this->ActiveDocument->GetHistory().CancelTransaction();
	this->ActiveDocument = nullptr;
	this->DragTargets.clear();
	this->ActiveHandle = TransformGizmoHandle::None;
	this->HasPublishedEdit = false;
}

TransformGizmoVisualState TransformGizmoController::BuildVisualState(const document::SceneDocument &Document, const Camera &Camera) const
{
	if (this->Operation == TransformGizmoOperation::Select)
		return {};
	// Drag constraints stay anchored to DragPivot/DragBasis, while the overlay
	// follows the transform command's current scene state. Keeping those two
	// responsibilities separate prevents pointer feedback from moving the
	// gesture's reference plane or axis.
	const bool Dragging = this->IsDragging();

	this->SelectedScratch.clear();
	Document.GetSelection().ResolveInto(Document.GetScene(), this->SelectedScratch);
	const std::vector<world::ObjectHandle> &Selected = this->SelectedScratch;
	std::vector<DragTarget> &Targets = this->VisualTargetsScratch;
	Targets.clear();
	Targets.reserve(Selected.size());
	const auto Access = Document.GetScene().Read();
	world::SceneTransformSnapshot::BuildInto(Access, this->TransformSnapshotScratch, this->TransformSnapshotBuildScratch);
	const world::SceneTransformSnapshot &WorldTransforms = this->TransformSnapshotScratch;
	for (const world::ObjectHandle Object : Selected)
	{
		const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Object);
		if (Identity.IsValid())
		{
			const components::CObjectIdentityComponent &IdentityComponent = Access.Resolve(Identity);
			if (IdentityComponent.IsLocked() || !IdentityComponent.IsEnabled() || !IdentityComponent.IsEditorVisible())
				return {};
		}
		const world::ComponentHandle<components::CObjectTransformComponent> Transform =
			Access.GetComponent<components::CObjectTransformComponent>(Object);
		if (!Transform.IsValid())
			continue;
		const components::CObjectTransformComponent &Component = Access.Resolve(Transform);
		const glm::mat4 InitialWorld = WorldTransforms.GetMatrix(Object);
		const auto [WorldBoundsMinimum, WorldBoundsMaximum] = CalculateWorldBounds(Access, Object, InitialWorld);
		Targets.push_back(
			{.Object = Object,
			 .Initial = {.Position = Component.GetPosition(), .Rotation = Component.GetRotation(), .Scale = Component.GetScale()},
			 .InitialWorld = InitialWorld,
			 .WorldBoundsMinimum = WorldBoundsMinimum,
			 .WorldBoundsMaximum = WorldBoundsMaximum});
	}
	if (Targets.empty())
		return {};
	const glm::vec3 Pivot = this->CalculatePivot(Document, Targets);
	return {.Visible = true,
			.Dragging = Dragging,
			.Pivot = Pivot,
			.Basis = this->CalculateBasis(Targets),
			.WorldScale = std::max(glm::distance(Camera.Position, Pivot) * 0.15f, 0.1f),
			.Operation = this->Operation,
			.ActiveHandle = Dragging ? this->ActiveHandle : TransformGizmoHandle::None};
}

TransformGizmoHandle TransformGizmoController::HitTest(const document::SceneDocument &Document, const Camera &Camera,
													   const core::WindowExtent Extent, const float32 NormalizedX,
													   const float32 NormalizedYFromTop, const float32 TolerancePixels) const
{
	if (!Extent.IsValid() || !std::isfinite(NormalizedX) || !std::isfinite(NormalizedYFromTop) || NormalizedX < 0.0f ||
		NormalizedX > 1.0f || NormalizedYFromTop < 0.0f || NormalizedYFromTop > 1.0f || !std::isfinite(TolerancePixels) ||
		TolerancePixels <= 0.0f)
	{
		throw std::invalid_argument("Transform-gizmo hit test requires valid normalized coordinates, extent, and tolerance");
	}
	const TransformGizmoVisualState State = this->BuildVisualState(Document, Camera);
	if (!State.Visible)
		return TransformGizmoHandle::None;
	const glm::mat4 ViewProjection = Camera.GetProjectionMatrix(Extent) * Camera.GetViewMatrix();
	const glm::vec2 Cursor{NormalizedX * static_cast<float32>(Extent.Width), NormalizedYFromTop * static_cast<float32>(Extent.Height)};

	if (State.Operation == TransformGizmoOperation::Universal)
	{
		glm::vec2 PivotPixels;
		if (ProjectToPixels(State.Pivot, ViewProjection, Extent, PivotPixels) && glm::distance(Cursor, PivotPixels) <= TolerancePixels)
		{
			return TransformGizmoHandle::ScaleUniform;
		}

		for (uint32 Axis = 0; Axis < 3; ++Axis)
		{
			glm::vec2 Endpoint;
			const glm::vec3 WorldEndpoint = State.Pivot + glm::normalize(State.Basis[Axis]) * State.WorldScale * 0.72f;
			if (ProjectToPixels(WorldEndpoint, ViewProjection, Extent, Endpoint) && glm::distance(Cursor, Endpoint) <= TolerancePixels)
			{
				return static_cast<TransformGizmoHandle>(static_cast<uint32>(TransformGizmoHandle::ScaleAxisX) + Axis);
			}
		}

		for (uint32 Plane = 0; Plane < 3; ++Plane)
		{
			const uint32 FirstAxis = Plane == 0 ? 0U : (Plane == 1 ? 1U : 2U);
			const uint32 SecondAxis = Plane == 0 ? 1U : (Plane == 1 ? 2U : 0U);
			const glm::vec3 First = glm::normalize(State.Basis[FirstAxis]) * State.WorldScale;
			const glm::vec3 Second = glm::normalize(State.Basis[SecondAxis]) * State.WorldScale;
			const std::array WorldCorners{State.Pivot + First * 0.48f + Second * 0.48f, State.Pivot + First * 0.61f + Second * 0.48f,
										  State.Pivot + First * 0.61f + Second * 0.61f, State.Pivot + First * 0.48f + Second * 0.61f};
			std::array<glm::vec2, 4> PixelCorners;
			bool Projected = true;
			for (uint32 Corner = 0; Corner < PixelCorners.size(); ++Corner)
				Projected = Projected && ProjectToPixels(WorldCorners[Corner], ViewProjection, Extent, PixelCorners[Corner]);
			if (Projected && PointInConvexQuad(Cursor, PixelCorners))
			{
				return static_cast<TransformGizmoHandle>(static_cast<uint32>(TransformGizmoHandle::ScalePlaneXY) + Plane);
			}
		}

		float32 BestRotationDistance = TolerancePixels;
		TransformGizmoHandle BestRotation = TransformGizmoHandle::None;
		for (uint32 Axis = 0; Axis < 4; ++Axis)
		{
			const glm::vec3 FirstDirection = Axis < 3 ? glm::normalize(State.Basis[(Axis + 1U) % 3U]) : glm::normalize(Camera.Right);
			const glm::vec3 SecondDirection = Axis < 3 ? glm::normalize(State.Basis[(Axis + 2U) % 3U]) : glm::normalize(Camera.Up);
			for (uint32 Arc = 0; Arc < 48; ++Arc)
			{
				const float32 FirstAngle = glm::two_pi<float32>() * static_cast<float32>(Arc) / 48.0f;
				const float32 SecondAngle = glm::two_pi<float32>() * static_cast<float32>(Arc + 1U) / 48.0f;
				const glm::vec3 FirstWorld =
					State.Pivot +
					(FirstDirection * std::cos(FirstAngle) + SecondDirection * std::sin(FirstAngle)) * State.WorldScale * 0.82f;
				const glm::vec3 SecondWorld =
					State.Pivot +
					(FirstDirection * std::cos(SecondAngle) + SecondDirection * std::sin(SecondAngle)) * State.WorldScale * 0.82f;
				glm::vec2 FirstPixels;
				glm::vec2 SecondPixels;
				if (!ProjectToPixels(FirstWorld, ViewProjection, Extent, FirstPixels) ||
					!ProjectToPixels(SecondWorld, ViewProjection, Extent, SecondPixels))
				{
					continue;
				}
				const float32 Distance = PointSegmentDistance(Cursor, FirstPixels, SecondPixels);
				if (Distance < BestRotationDistance)
				{
					BestRotationDistance = Distance;
					BestRotation = static_cast<TransformGizmoHandle>(static_cast<uint32>(TransformGizmoHandle::RotateAxisX) + Axis);
				}
			}
		}
		if (BestRotation != TransformGizmoHandle::None)
			return BestRotation;
	}

	if (State.Operation == TransformGizmoOperation::Rotate)
	{
		float32 BestDistance = TolerancePixels;
		TransformGizmoHandle Best = TransformGizmoHandle::None;
		for (uint32 Axis = 0; Axis < 4; ++Axis)
		{
			const glm::vec3 FirstDirection = Axis < 3 ? glm::normalize(State.Basis[(Axis + 1U) % 3U]) : glm::normalize(Camera.Right);
			const glm::vec3 SecondDirection = Axis < 3 ? glm::normalize(State.Basis[(Axis + 2U) % 3U]) : glm::normalize(Camera.Up);
			for (uint32 Arc = 0; Arc < 48; ++Arc)
			{
				const float32 FirstAngle = glm::two_pi<float32>() * static_cast<float32>(Arc) / 48.0f;
				const float32 SecondAngle = glm::two_pi<float32>() * static_cast<float32>(Arc + 1U) / 48.0f;
				const glm::vec3 FirstWorld =
					State.Pivot + (FirstDirection * std::cos(FirstAngle) + SecondDirection * std::sin(FirstAngle)) * State.WorldScale;
				const glm::vec3 SecondWorld =
					State.Pivot + (FirstDirection * std::cos(SecondAngle) + SecondDirection * std::sin(SecondAngle)) * State.WorldScale;
				glm::vec2 FirstPixels;
				glm::vec2 SecondPixels;
				if (!ProjectToPixels(FirstWorld, ViewProjection, Extent, FirstPixels) ||
					!ProjectToPixels(SecondWorld, ViewProjection, Extent, SecondPixels))
				{
					continue;
				}
				const float32 Distance = PointSegmentDistance(Cursor, FirstPixels, SecondPixels);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					Best = Axis < 3 ? static_cast<TransformGizmoHandle>(Axis + 1U) : TransformGizmoHandle::Screen;
				}
			}
		}
		return Best;
	}

	if (State.Operation == TransformGizmoOperation::Scale)
	{
		glm::vec2 PivotPixels;
		if (ProjectToPixels(State.Pivot, ViewProjection, Extent, PivotPixels) && glm::distance(Cursor, PivotPixels) <= TolerancePixels)
		{
			return TransformGizmoHandle::Uniform;
		}
	}

	for (uint32 Plane = 0; Plane < 3; ++Plane)
	{
		const uint32 FirstAxis = Plane == 0 ? 0U : (Plane == 1 ? 1U : 2U);
		const uint32 SecondAxis = Plane == 0 ? 1U : (Plane == 1 ? 2U : 0U);
		const glm::vec3 First = glm::normalize(State.Basis[FirstAxis]) * State.WorldScale;
		const glm::vec3 Second = glm::normalize(State.Basis[SecondAxis]) * State.WorldScale;
		const std::array WorldCorners{State.Pivot + First * 0.20f + Second * 0.20f, State.Pivot + First * 0.42f + Second * 0.20f,
									  State.Pivot + First * 0.42f + Second * 0.42f, State.Pivot + First * 0.20f + Second * 0.42f};
		std::array<glm::vec2, 4> PixelCorners;
		bool Projected = true;
		for (uint32 Corner = 0; Corner < PixelCorners.size(); ++Corner)
			Projected = Projected && ProjectToPixels(WorldCorners[Corner], ViewProjection, Extent, PixelCorners[Corner]);
		if (Projected && PointInConvexQuad(Cursor, PixelCorners))
			return static_cast<TransformGizmoHandle>(Plane + 4U);
	}

	float32 BestDistance = TolerancePixels;
	TransformGizmoHandle Best = TransformGizmoHandle::None;
	for (uint32 Axis = 0; Axis < 3; ++Axis)
	{
		glm::vec2 Start;
		glm::vec2 End;
		if (!ProjectToPixels(State.Pivot, ViewProjection, Extent, Start) ||
			!ProjectToPixels(State.Pivot + glm::normalize(State.Basis[Axis]) * State.WorldScale, ViewProjection, Extent, End))
		{
			continue;
		}
		const float32 Distance = PointSegmentDistance(Cursor, Start, End);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = static_cast<TransformGizmoHandle>(Axis + 1U);
		}
	}
	return Best;
}

bool TransformGizmoController::IsDragging() const noexcept
{
	return this->ActiveDocument != nullptr;
}

TransformGizmoOperation TransformGizmoController::GetOperation() const noexcept
{
	return this->Operation;
}

TransformGizmoSpace TransformGizmoController::GetSpace() const noexcept
{
	return this->Space;
}

TransformGizmoPivot TransformGizmoController::GetPivot() const noexcept
{
	return this->PivotMode;
}

const TransformGizmoSnapSettings &TransformGizmoController::GetSnapSettings() const noexcept
{
	return this->SnapSettings;
}

TransformGizmoController::Ray TransformGizmoController::BuildRay(const Camera &Camera, const core::WindowExtent Extent,
																 const float32 NormalizedX, const float32 NormalizedYFromTop)
{
	if (!Extent.IsValid() || !std::isfinite(NormalizedX) || !std::isfinite(NormalizedYFromTop) || NormalizedX < 0.0f ||
		NormalizedX > 1.0f || NormalizedYFromTop < 0.0f || NormalizedYFromTop > 1.0f)
	{
		throw std::invalid_argument("Transform-gizmo ray requires a valid extent and normalized cursor coordinates");
	}
	const float32 X = NormalizedX * 2.0f - 1.0f;
	const float32 Y = 1.0f - NormalizedYFromTop * 2.0f;
	const glm::mat4 InverseViewProjection = glm::inverse(Camera.GetProjectionMatrix(Extent) * Camera.GetViewMatrix());
	glm::vec4 Near = InverseViewProjection * glm::vec4(X, Y, 1.0f, 1.0f);
	glm::vec4 Far = InverseViewProjection * glm::vec4(X, Y, 0.0f, 1.0f);
	Near /= Near.w;
	Far /= Far.w;
	const glm::vec3 Direction = glm::normalize(glm::vec3(Far - Near));
	if (!IsFinite(Direction))
		throw std::runtime_error("Transform-gizmo ray unprojection produced a non-finite direction");
	return {.Origin = glm::vec3(Near), .Direction = Direction};
}

bool TransformGizmoController::IntersectPlane(const Ray &Ray, const glm::vec3 &Point, const glm::vec3 &Normal, glm::vec3 &Intersection)
{
	const float32 Denominator = glm::dot(Ray.Direction, Normal);
	if (std::abs(Denominator) <= MinimumMagnitude)
		return false;
	const float32 Distance = glm::dot(Point - Ray.Origin, Normal) / Denominator;
	Intersection = Ray.Origin + Ray.Direction * Distance;
	return std::isfinite(Distance) && IsFinite(Intersection);
}

float32 TransformGizmoController::ClosestAxisParameter(const Ray &Ray, const glm::vec3 &Origin, const glm::vec3 &Axis,
													   const glm::vec3 &ViewDirection)
{
	const glm::vec3 Offset = Ray.Origin - Origin;
	const float32 Alignment = glm::dot(Ray.Direction, Axis);
	const float32 Denominator = 1.0f - Alignment * Alignment;
	if (Denominator > MinimumMagnitude)
		return (glm::dot(Axis, Offset) - Alignment * glm::dot(Ray.Direction, Offset)) / Denominator;

	glm::vec3 PlaneNormal = glm::cross(Axis, glm::cross(ViewDirection, Axis));
	if (glm::dot(PlaneNormal, PlaneNormal) <= MinimumMagnitude)
		PlaneNormal = glm::cross(Axis, glm::vec3(0.0f, 1.0f, 0.0f));
	PlaneNormal = glm::normalize(PlaneNormal);
	glm::vec3 Intersection;
	if (!IntersectPlane(Ray, Origin, PlaneNormal, Intersection))
		return 0.0f;
	return glm::dot(Intersection - Origin, Axis);
}

float32 TransformGizmoController::Snap(const float32 Value, const float32 Increment)
{
	return std::round(Value / Increment) * Increment;
}

glm::vec3 TransformGizmoController::AxisForHandle(const TransformGizmoHandle Handle, const glm::mat3 &Basis)
{
	switch (Handle)
	{
	case TransformGizmoHandle::AxisX:
	case TransformGizmoHandle::RotateAxisX:
	case TransformGizmoHandle::ScaleAxisX:
		return glm::normalize(Basis[0]);
	case TransformGizmoHandle::AxisY:
	case TransformGizmoHandle::RotateAxisY:
	case TransformGizmoHandle::ScaleAxisY:
		return glm::normalize(Basis[1]);
	case TransformGizmoHandle::AxisZ:
	case TransformGizmoHandle::RotateAxisZ:
	case TransformGizmoHandle::ScaleAxisZ:
		return glm::normalize(Basis[2]);
	default:
		return glm::vec3(0.0f);
	}
}

glm::vec3 TransformGizmoController::PlaneNormalForHandle(const TransformGizmoHandle Handle, const glm::mat3 &Basis, const Camera &Camera)
{
	switch (Handle)
	{
	case TransformGizmoHandle::PlaneXY:
	case TransformGizmoHandle::ScalePlaneXY:
		return glm::normalize(Basis[2]);
	case TransformGizmoHandle::PlaneYZ:
	case TransformGizmoHandle::ScalePlaneYZ:
		return glm::normalize(Basis[0]);
	case TransformGizmoHandle::PlaneZX:
	case TransformGizmoHandle::ScalePlaneZX:
		return glm::normalize(Basis[1]);
	case TransformGizmoHandle::AxisX:
	case TransformGizmoHandle::RotateAxisX:
	case TransformGizmoHandle::ScaleAxisX:
		return glm::normalize(Basis[0]);
	case TransformGizmoHandle::AxisY:
	case TransformGizmoHandle::RotateAxisY:
	case TransformGizmoHandle::ScaleAxisY:
		return glm::normalize(Basis[1]);
	case TransformGizmoHandle::AxisZ:
	case TransformGizmoHandle::RotateAxisZ:
	case TransformGizmoHandle::ScaleAxisZ:
		return glm::normalize(Basis[2]);
	case TransformGizmoHandle::Screen:
	case TransformGizmoHandle::Uniform:
	case TransformGizmoHandle::RotateScreen:
	case TransformGizmoHandle::ScaleUniform:
		return glm::normalize(Camera.Front);
	default:
		return glm::vec3(0.0f);
	}
}

commands::TransformState TransformGizmoController::ResolveLocalTransform(const DragTarget &Target, const glm::mat4 &DesiredWorld)
{
	const world::DecomposedTransform Local = world::DecomposeAffineTransform(Target.ParentWorldInverse * DesiredWorld);
	return {.Position = Local.Position, .Rotation = Local.Rotation, .Scale = Local.Scale};
}

glm::vec3 TransformGizmoController::CalculatePivot(const document::SceneDocument &Document, const std::vector<DragTarget> &Targets) const
{
	if (Targets.empty())
		return glm::vec3(0.0f);
	switch (this->PivotMode)
	{
	case TransformGizmoPivot::MedianPoint:
	{
		glm::vec3 Sum(0.0f);
		for (const DragTarget &Target : Targets)
			Sum += glm::vec3(Target.InitialWorld[3]);
		return Sum / static_cast<float32>(Targets.size());
	}
	case TransformGizmoPivot::BoundingBoxCenter:
	{
		glm::vec3 Minimum(std::numeric_limits<float32>::max());
		glm::vec3 Maximum(std::numeric_limits<float32>::lowest());
		for (const DragTarget &Target : Targets)
		{
			Minimum = glm::min(Minimum, Target.WorldBoundsMinimum);
			Maximum = glm::max(Maximum, Target.WorldBoundsMaximum);
		}
		return (Minimum + Maximum) * 0.5f;
	}
	case TransformGizmoPivot::WorldOrigin:
		return glm::vec3(0.0f);
	case TransformGizmoPivot::ActiveObject:
	case TransformGizmoPivot::IndividualOrigins:
		break;
	default:
		throw std::logic_error("Transform-gizmo pivot mode is invalid");
	}
	const world::ObjectHandle Primary = Document.GetScene().FindObject(Document.GetSelection().GetPrimary());
	const auto PrimaryTarget =
		std::find_if(Targets.begin(), Targets.end(), [Primary](const DragTarget &Target) { return Target.Object == Primary; });
	return glm::vec3((PrimaryTarget != Targets.end() ? PrimaryTarget->InitialWorld : Targets.back().InitialWorld)[3]);
}

glm::mat3 TransformGizmoController::CalculateBasis(const std::vector<DragTarget> &Targets) const
{
	if (this->Space == TransformGizmoSpace::World || Targets.empty())
		return glm::mat3(1.0f);
	const glm::mat3 Source(Targets.back().InitialWorld);
	const glm::vec3 AxisX = glm::normalize(Source[0]);
	const glm::vec3 AxisY = glm::normalize(Source[1] - AxisX * glm::dot(AxisX, Source[1]));
	glm::vec3 AxisZ = glm::normalize(glm::cross(AxisX, AxisY));
	if (glm::dot(AxisZ, Source[2]) < 0.0f)
		AxisZ = -AxisZ;
	if (!IsFinite(AxisX) || !IsFinite(AxisY) || !IsFinite(AxisZ))
		throw world::SceneException("Transform-gizmo local basis is degenerate");
	return glm::mat3(AxisX, AxisY, AxisZ);
}

void TransformGizmoController::ApplyTranslation(const Ray &Ray)
{
	glm::vec3 Delta(0.0f);
	if (IsAxisHandle(this->ActiveHandle))
	{
		float32 AxisDelta = ClosestAxisParameter(Ray, this->DragPivot, this->ConstraintAxis, Ray.Direction) - this->StartAxisParameter;
		if (this->SnapSettings.Enabled)
			AxisDelta = Snap(AxisDelta, this->SnapSettings.Translation);
		Delta = this->ConstraintAxis * AxisDelta;
	}
	else
	{
		glm::vec3 Current;
		if (!IntersectPlane(Ray, this->DragPivot, this->ConstraintPlaneNormal, Current))
			return;
		Delta = Current - this->StartPoint;
		if (this->SnapSettings.Enabled)
		{
			glm::vec3 LocalDelta = glm::transpose(this->DragBasis) * Delta;
			const glm::vec3 Enabled =
				this->ActiveHandle == TransformGizmoHandle::Screen ? glm::vec3(1.0f) : EnabledScaleAxes(this->ActiveHandle);
			for (uint32 Axis = 0; Axis < 3; ++Axis)
				LocalDelta[Axis] = Enabled[Axis] == 0.0f ? 0.0f : Snap(LocalDelta[Axis], this->SnapSettings.Translation);
			Delta = this->DragBasis * LocalDelta;
		}
	}

	std::vector<commands::TransformEditTarget> &Targets = this->TransformEditTargetsScratch;
	Targets.clear();
	Targets.reserve(this->DragTargets.size());
	for (const DragTarget &Target : this->DragTargets)
	{
		commands::TransformState After = Target.Initial;
		const glm::vec3 WorldPosition = glm::vec3(Target.InitialWorld[3]) + Delta;
		After.Position = glm::vec3(Target.ParentWorldInverse * glm::vec4(WorldPosition, 1.0f));
		Targets.push_back({.Object = Target.Object, .Before = Target.Initial, .After = After});
	}
	this->Publish(Targets);
}

void TransformGizmoController::ApplyRotation(const Ray &Ray)
{
	glm::vec3 CurrentPoint;
	if (!IntersectPlane(Ray, this->DragPivot, this->ConstraintPlaneNormal, CurrentPoint))
		return;
	glm::vec3 StartVector = this->StartPoint - this->DragPivot;
	glm::vec3 CurrentVector = CurrentPoint - this->DragPivot;
	if (glm::dot(StartVector, StartVector) <= MinimumMagnitude || glm::dot(CurrentVector, CurrentVector) <= MinimumMagnitude)
		return;
	StartVector = glm::normalize(StartVector);
	CurrentVector = glm::normalize(CurrentVector);
	float32 Angle =
		std::atan2(glm::dot(this->ConstraintPlaneNormal, glm::cross(StartVector, CurrentVector)), glm::dot(StartVector, CurrentVector));
	if (this->SnapSettings.Enabled)
		Angle = glm::radians(Snap(glm::degrees(Angle), this->SnapSettings.RotationDegrees));
	const glm::quat RotationDelta = glm::angleAxis(Angle, glm::normalize(this->ConstraintPlaneNormal));

	std::vector<commands::TransformEditTarget> &Targets = this->TransformEditTargetsScratch;
	Targets.clear();
	Targets.reserve(this->DragTargets.size());
	for (const DragTarget &Target : this->DragTargets)
	{
		const glm::vec3 Pivot =
			this->PivotMode == TransformGizmoPivot::IndividualOrigins ? glm::vec3(Target.InitialWorld[3]) : this->DragPivot;
		const glm::mat4 WorldDelta =
			glm::translate(glm::mat4(1.0f), Pivot) * glm::mat4_cast(RotationDelta) * glm::translate(glm::mat4(1.0f), -Pivot);
		const commands::TransformState After = ResolveLocalTransform(Target, WorldDelta * Target.InitialWorld);
		Targets.push_back({.Object = Target.Object, .Before = Target.Initial, .After = After});
	}
	this->Publish(Targets);
}

void TransformGizmoController::ApplyScale(const Ray &Ray, const Camera &Camera)
{
	glm::vec3 Factors(1.0f);
	const glm::vec3 Enabled = EnabledScaleAxes(this->ActiveHandle);
	if (IsAxisHandle(this->ActiveHandle))
	{
		const float32 Delta = ClosestAxisParameter(Ray, this->DragPivot, this->ConstraintAxis, Camera.Front) - this->StartAxisParameter;
		float32 Factor = 1.0f + Delta / this->WorldScale;
		if (this->SnapSettings.Enabled)
			Factor = 1.0f + Snap(Factor - 1.0f, this->SnapSettings.Scale);
		Factors += Enabled * (Factor - 1.0f);
	}
	else
	{
		glm::vec3 Current;
		if (!IntersectPlane(Ray, this->DragPivot, this->ConstraintPlaneNormal, Current))
			return;
		const glm::vec3 LocalDelta = glm::transpose(this->DragBasis) * (Current - this->StartPoint);
		float32 UniformDelta = glm::dot(Current - this->StartPoint, Camera.Up) / this->WorldScale;
		for (uint32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Enabled[Axis] == 0.0f)
				continue;
			float32 Factor = this->ActiveHandle == TransformGizmoHandle::Uniform || this->ActiveHandle == TransformGizmoHandle::ScaleUniform
								 ? 1.0f + UniformDelta
								 : 1.0f + LocalDelta[Axis] / this->WorldScale;
			if (this->SnapSettings.Enabled)
				Factor = 1.0f + Snap(Factor - 1.0f, this->SnapSettings.Scale);
			Factors[Axis] = Factor;
		}
	}
	if (!IsFinite(Factors))
		return;
	Factors = glm::max(Factors, glm::vec3(MinimumInteractiveScaleFactor));

	std::vector<commands::TransformEditTarget> &Targets = this->TransformEditTargetsScratch;
	Targets.clear();
	Targets.reserve(this->DragTargets.size());
	try
	{
		for (const DragTarget &Target : this->DragTargets)
		{
			const glm::vec3 Pivot =
				this->PivotMode == TransformGizmoPivot::IndividualOrigins ? glm::vec3(Target.InitialWorld[3]) : this->DragPivot;
			glm::mat4 Basis(1.0f);
			Basis[0] = glm::vec4(this->DragBasis[0], 0.0f);
			Basis[1] = glm::vec4(this->DragBasis[1], 0.0f);
			Basis[2] = glm::vec4(this->DragBasis[2], 0.0f);
			const glm::mat4 WorldDelta = glm::translate(glm::mat4(1.0f), Pivot) * Basis * glm::scale(glm::mat4(1.0f), Factors) *
										 glm::transpose(Basis) * glm::translate(glm::mat4(1.0f), -Pivot);
			const commands::TransformState After = ResolveLocalTransform(Target, WorldDelta * Target.InitialWorld);
			Targets.push_back({.Object = Target.Object, .Before = Target.Initial, .After = After});
		}
	}
	catch (const std::invalid_argument &)
	{
		// A pointer drag may cross the singular zero-scale boundary or request a
		// world-space scale that would introduce shear under a parent. Preserve
		// the last valid transaction state instead of terminating the editor.
		return;
	}
	this->Publish(Targets);
}

void TransformGizmoController::Publish(const std::span<const commands::TransformEditTarget> Targets)
{
	this->ActiveDocument->GetHistory().Execute(
		commands::TransformEditCommand::Create(this->ActiveDocument->GetScene(), Targets, "Transform selection"));
	this->HasPublishedEdit = true;
}
} // namespace editor::viewport
