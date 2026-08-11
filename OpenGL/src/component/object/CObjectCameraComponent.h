#pragma once

#include "src/component/object/CObjectComponent.h"
#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/types.h"

namespace components
{
enum class CameraProjection : uint8
{
	Perspective,
	Orthographic
};

class ENGINE_API CObjectCameraComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<CObjectIdentityComponent, CObjectTransformComponent>;

	explicit CObjectCameraComponent(world::ObjectHandle Owner) noexcept;
	CCOMPONENT_BODY(CObjectCameraComponent)

	[[nodiscard]] CameraProjection GetProjection() const noexcept;
	void SetProjection(CameraProjection Projection) noexcept;
	[[nodiscard]] float32 GetVerticalFieldOfViewDegrees() const noexcept;
	void SetVerticalFieldOfViewDegrees(float32 Degrees);
	[[nodiscard]] float32 GetOrthographicHeight() const noexcept;
	void SetOrthographicHeight(float32 Height);
	[[nodiscard]] float32 GetNearPlane() const noexcept;
	[[nodiscard]] float32 GetFarPlane() const noexcept;
	void SetClipPlanes(float32 NearPlane, float32 FarPlane);
	[[nodiscard]] float32 GetExposureCompensation() const noexcept;
	void SetExposureCompensation(float32 Stops) noexcept;
	[[nodiscard]] bool IsPrimary() const noexcept;
	void SetPrimary(bool Primary) noexcept;
	[[nodiscard]] bool IsTemporalJitterEnabled() const noexcept;
	void SetTemporalJitterEnabled(bool Enabled) noexcept;

  private:
	CameraProjection Projection = CameraProjection::Perspective;
	float32 VerticalFieldOfViewDegrees = 60.0f;
	float32 OrthographicHeight = 10.0f;
	float32 NearPlane = 0.05f;
	float32 FarPlane = 100'000.0f;
	float32 ExposureCompensation = 0.0f;
	bool Primary = false;
	bool TemporalJitterEnabled = true;
};
} // namespace components
