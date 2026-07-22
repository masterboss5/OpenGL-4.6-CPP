#pragma once
#include "src/component/object/CObjectComponent.h"
#include "src/types.h"

#include <glm.hpp>
#include <gtc/quaternion.hpp>

// TODO
//============================================================
/*
	1. Decide where to add grid snapping, as another component or inside this TransformComponenet
	2. Add macros to enable or disable [[unlikely]]
	3. add [[nodiscard]]
	4. add noexcept
	5. maybe: add setFromMatrix()
	6. add rotateX, etc
*/
//============================================================

namespace components
{
class CObjectTransformComponent final : public CObjectComponent
{
  private:
	glm::vec3 Position{0.0f, 0.0f, 0.0f};
	glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 Scale{1.0f, 1.0f, 1.0f};
	mutable bool NeedsRecalculation{true};
	mutable glm::mat4 Matrix{1.0f};

	void UpdateMatrix() const;
	void RecalculateMatrix() const;

  public:
	explicit CObjectTransformComponent(world::ObjectHandle Owner, const glm::vec3 &Position = glm::vec3{0.0f, 0.0f, 0.0f},
									   const glm::quat &Rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
									   const glm::vec3 &Scale = glm::vec3{1.0f, 1.0f, 1.0f});
	using Dependencies = TypeList<>;
	CCOMPONENT_BODY(CObjectTransformComponent)

	void ResetTransform();
	void ResetPosition();
	void ResetRotation();
	void ResetScale();

	void SetTransform(const glm::vec3 &Position, const glm::quat &Rotation, const glm::vec3 &Scale);

	void SetPosition(const glm::vec3 &Position);
	[[nodiscard]] glm::vec3 GetPosition() const;
	void Translate(const glm::vec3 &Translation);
	void TranslateX(float32 X);
	void TranslateY(float32 Y);
	void TranslateZ(float32 Z);
	void LerpPosition(const glm::vec3 &Target, float32 Alpha);
	void SetPositionX(float32 X);
	void SetPositionY(float32 Y);
	void SetPositionZ(float32 Z);

	void SetRotation(const glm::quat &Quat);
	void SetRotationEuler(const glm::vec3 &EulerAngles);
	void Rotate(float32 AngleDegrees, const glm::vec3 &Axis);
	[[nodiscard]] glm::quat GetRotation() const;
	[[nodiscard]] glm::vec3 GetRotationEuler() const;
	void SlerpRotation(const glm::quat &Target, float32 Alpha);

	void SetScale(const glm::vec3 &Scale);
	void SetScale(float32 Scale);
	[[nodiscard]] glm::vec3 GetScale() const;
	void LerpScale(const glm::vec3 &Target, float32 Alpha);
	void SetScaleX(float32 X);
	void SetScaleY(float32 Y);
	void SetScaleZ(float32 Z);

	void LookAt(const glm::vec3 &Target, const glm::vec3 &Up = glm::vec3{0.0f, 1.0f, 0.0f});
	[[nodiscard]] float32 DistanceTo(const glm::vec3 &Point) const;
	[[nodiscard]] float32 DistanceToSquared(const glm::vec3 &Point) const;
	[[nodiscard]] bool IsWithinDistance(const glm::vec3 &Point, float32 Distance) const;
	[[nodiscard]] glm::vec3 GetForward() const;
	[[nodiscard]] glm::vec3 GetUp() const;
	[[nodiscard]] glm::vec3 GetRight() const;
	[[nodiscard]] const glm::mat4 &GetMatrix() const;

	void OnAttachment() override;
	void OnDetachment() override;
};
} // namespace components
