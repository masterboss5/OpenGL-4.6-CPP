#include "CObjectLightComponents.h"

#include <stdexcept>

namespace
{
void ValidateColor(const glm::vec3 &Color)
{
	if (Color.r < 0.0f || Color.g < 0.0f || Color.b < 0.0f)
		throw std::out_of_range("Physical light color cannot contain negative radiance");
}

void ValidatePositive(const float32 Value, const string_view Name)
{
	if (Value <= 0.0f)
		throw std::out_of_range(string(Name) + " must be positive");
}
} // namespace

namespace components
{
CObjectPointLightComponent::CObjectPointLightComponent(const world::ObjectHandle Owner) noexcept : CObjectComponent(Owner)
{
}

const glm::vec3 &CObjectPointLightComponent::GetColor() const noexcept
{
	return this->Color;
}

void CObjectPointLightComponent::SetColor(const glm::vec3 &LinearColor)
{
	ValidateColor(LinearColor);
	this->Color = LinearColor;
}

float32 CObjectPointLightComponent::GetLuminousPowerLumens() const noexcept
{
	return this->LuminousPowerLumens;
}

void CObjectPointLightComponent::SetLuminousPowerLumens(const float32 Lumens)
{
	ValidatePositive(Lumens, "Point-light luminous power");
	this->LuminousPowerLumens = Lumens;
}

float32 CObjectPointLightComponent::GetRange() const noexcept
{
	return this->Range;
}

void CObjectPointLightComponent::SetRange(const float32 Range)
{
	ValidatePositive(Range, "Point-light range");
	this->Range = Range;
}

float32 CObjectPointLightComponent::GetSourceRadius() const noexcept
{
	return this->SourceRadius;
}

void CObjectPointLightComponent::SetSourceRadius(const float32 Radius)
{
	if (Radius < 0.0f)
		throw std::out_of_range("Point-light source radius cannot be negative");
	this->SourceRadius = Radius;
}

LightShadowSettings &CObjectPointLightComponent::GetShadowSettings() noexcept
{
	return this->Shadows;
}

const LightShadowSettings &CObjectPointLightComponent::GetShadowSettings() const noexcept
{
	return this->Shadows;
}

CObjectSpotLightComponent::CObjectSpotLightComponent(const world::ObjectHandle Owner) noexcept : CObjectComponent(Owner)
{
}

const glm::vec3 &CObjectSpotLightComponent::GetColor() const noexcept
{
	return this->Color;
}

void CObjectSpotLightComponent::SetColor(const glm::vec3 &LinearColor)
{
	ValidateColor(LinearColor);
	this->Color = LinearColor;
}

float32 CObjectSpotLightComponent::GetLuminousPowerLumens() const noexcept
{
	return this->LuminousPowerLumens;
}

void CObjectSpotLightComponent::SetLuminousPowerLumens(const float32 Lumens)
{
	ValidatePositive(Lumens, "Spot-light luminous power");
	this->LuminousPowerLumens = Lumens;
}

float32 CObjectSpotLightComponent::GetRange() const noexcept
{
	return this->Range;
}

void CObjectSpotLightComponent::SetRange(const float32 Range)
{
	ValidatePositive(Range, "Spot-light range");
	this->Range = Range;
}

float32 CObjectSpotLightComponent::GetInnerConeDegrees() const noexcept
{
	return this->InnerConeDegrees;
}

float32 CObjectSpotLightComponent::GetOuterConeDegrees() const noexcept
{
	return this->OuterConeDegrees;
}

void CObjectSpotLightComponent::SetConeAngles(const float32 InnerDegrees, const float32 OuterDegrees)
{
	if (InnerDegrees < 0.0f || OuterDegrees <= 0.0f || InnerDegrees > OuterDegrees || OuterDegrees >= 180.0f)
		throw std::out_of_range("Spot-light cones require 0 <= inner <= outer < 180 degrees");
	this->InnerConeDegrees = InnerDegrees;
	this->OuterConeDegrees = OuterDegrees;
}

LightShadowSettings &CObjectSpotLightComponent::GetShadowSettings() noexcept
{
	return this->Shadows;
}

const LightShadowSettings &CObjectSpotLightComponent::GetShadowSettings() const noexcept
{
	return this->Shadows;
}

CObjectDirectionalLightComponent::CObjectDirectionalLightComponent(const world::ObjectHandle Owner) noexcept : CObjectComponent(Owner)
{
}

const glm::vec3 &CObjectDirectionalLightComponent::GetColor() const noexcept
{
	return this->Color;
}

void CObjectDirectionalLightComponent::SetColor(const glm::vec3 &LinearColor)
{
	ValidateColor(LinearColor);
	this->Color = LinearColor;
}

float32 CObjectDirectionalLightComponent::GetIlluminanceLux() const noexcept
{
	return this->IlluminanceLux;
}

void CObjectDirectionalLightComponent::SetIlluminanceLux(const float32 Lux)
{
	ValidatePositive(Lux, "Directional-light illuminance");
	this->IlluminanceLux = Lux;
}

float32 CObjectDirectionalLightComponent::GetAngularDiameterDegrees() const noexcept
{
	return this->AngularDiameterDegrees;
}

void CObjectDirectionalLightComponent::SetAngularDiameterDegrees(const float32 Degrees)
{
	if (Degrees < 0.0f || Degrees >= 180.0f)
		throw std::out_of_range("Directional-light angular diameter must be in [0, 180) degrees");
	this->AngularDiameterDegrees = Degrees;
}

uint32 CObjectDirectionalLightComponent::GetCascadeCount() const noexcept
{
	return this->CascadeCount;
}

void CObjectDirectionalLightComponent::SetCascadeCount(const uint32 Count)
{
	if (Count == 0 || Count > 8)
		throw std::out_of_range("Directional-light cascade count must be between one and eight");
	this->CascadeCount = Count;
}

float32 CObjectDirectionalLightComponent::GetCascadeDistributionExponent() const noexcept
{
	return this->CascadeDistributionExponent;
}

void CObjectDirectionalLightComponent::SetCascadeDistributionExponent(const float32 Exponent)
{
	ValidatePositive(Exponent, "Directional-light cascade distribution exponent");
	this->CascadeDistributionExponent = Exponent;
}

LightShadowSettings &CObjectDirectionalLightComponent::GetShadowSettings() noexcept
{
	return this->Shadows;
}

const LightShadowSettings &CObjectDirectionalLightComponent::GetShadowSettings() const noexcept
{
	return this->Shadows;
}
} // namespace components
