#include "CObjectCameraComponent.h"

#include <stdexcept>

namespace components
{
CObjectCameraComponent::CObjectCameraComponent(const world::ObjectHandle Owner) noexcept : CObjectComponent(Owner)
{
}

CameraProjection CObjectCameraComponent::GetProjection() const noexcept
{
	return this->Projection;
}

void CObjectCameraComponent::SetProjection(const CameraProjection Projection) noexcept
{
	this->Projection = Projection;
}

float32 CObjectCameraComponent::GetVerticalFieldOfViewDegrees() const noexcept
{
	return this->VerticalFieldOfViewDegrees;
}

void CObjectCameraComponent::SetVerticalFieldOfViewDegrees(const float32 Degrees)
{
	if (Degrees <= 0.0f || Degrees >= 180.0f)
		throw std::out_of_range("Camera field of view must be between zero and 180 degrees");
	this->VerticalFieldOfViewDegrees = Degrees;
}

float32 CObjectCameraComponent::GetOrthographicHeight() const noexcept
{
	return this->OrthographicHeight;
}

void CObjectCameraComponent::SetOrthographicHeight(const float32 Height)
{
	if (Height <= 0.0f)
		throw std::out_of_range("Camera orthographic height must be positive");
	this->OrthographicHeight = Height;
}

float32 CObjectCameraComponent::GetNearPlane() const noexcept
{
	return this->NearPlane;
}

float32 CObjectCameraComponent::GetFarPlane() const noexcept
{
	return this->FarPlane;
}

void CObjectCameraComponent::SetClipPlanes(const float32 NearPlane, const float32 FarPlane)
{
	if (NearPlane <= 0.0f || FarPlane <= NearPlane)
		throw std::out_of_range("Camera clip planes require 0 < near < far");
	this->NearPlane = NearPlane;
	this->FarPlane = FarPlane;
}

float32 CObjectCameraComponent::GetExposureCompensation() const noexcept
{
	return this->ExposureCompensation;
}

void CObjectCameraComponent::SetExposureCompensation(const float32 Stops) noexcept
{
	this->ExposureCompensation = Stops;
}

bool CObjectCameraComponent::IsPrimary() const noexcept
{
	return this->Primary;
}

void CObjectCameraComponent::SetPrimary(const bool Primary) noexcept
{
	this->Primary = Primary;
}

bool CObjectCameraComponent::IsTemporalJitterEnabled() const noexcept
{
	return this->TemporalJitterEnabled;
}

void CObjectCameraComponent::SetTemporalJitterEnabled(const bool Enabled) noexcept
{
	this->TemporalJitterEnabled = Enabled;
}
} // namespace components
