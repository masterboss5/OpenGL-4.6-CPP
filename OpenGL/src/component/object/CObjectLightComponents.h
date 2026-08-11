#pragma once

#include "src/component/object/CObjectComponent.h"
#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/types.h"

#include <glm.hpp>

namespace components
{
enum class ShadowResolution : uint16
{
	Resolution256 = 256,
	Resolution512 = 512,
	Resolution1024 = 1'024,
	Resolution2048 = 2'048,
	Resolution4096 = 4'096,
	Resolution8192 = 8'192
};

struct LightShadowSettings final
{
	bool CastShadows = true;
	ShadowResolution Resolution = ShadowResolution::Resolution2048;
	float32 ConstantBias = 0.0005f;
	float32 SlopeBias = 1.5f;
	float32 NormalBias = 0.02f;
	float32 FilterRadius = 1.0f;
};

class ENGINE_API CObjectPointLightComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<CObjectIdentityComponent, CObjectTransformComponent>;

	explicit CObjectPointLightComponent(world::ObjectHandle Owner) noexcept;
	CCOMPONENT_BODY(CObjectPointLightComponent)

	[[nodiscard]] const glm::vec3 &GetColor() const noexcept;
	void SetColor(const glm::vec3 &LinearColor);
	[[nodiscard]] float32 GetLuminousPowerLumens() const noexcept;
	void SetLuminousPowerLumens(float32 Lumens);
	[[nodiscard]] float32 GetRange() const noexcept;
	void SetRange(float32 Range);
	[[nodiscard]] float32 GetSourceRadius() const noexcept;
	void SetSourceRadius(float32 Radius);
	[[nodiscard]] LightShadowSettings &GetShadowSettings() noexcept;
	[[nodiscard]] const LightShadowSettings &GetShadowSettings() const noexcept;

  private:
	glm::vec3 Color{1.0f};
	float32 LuminousPowerLumens = 1'500.0f;
	float32 Range = 20.0f;
	float32 SourceRadius = 0.0f;
	LightShadowSettings Shadows;
};

class ENGINE_API CObjectSpotLightComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<CObjectIdentityComponent, CObjectTransformComponent>;

	explicit CObjectSpotLightComponent(world::ObjectHandle Owner) noexcept;
	CCOMPONENT_BODY(CObjectSpotLightComponent)

	[[nodiscard]] const glm::vec3 &GetColor() const noexcept;
	void SetColor(const glm::vec3 &LinearColor);
	[[nodiscard]] float32 GetLuminousPowerLumens() const noexcept;
	void SetLuminousPowerLumens(float32 Lumens);
	[[nodiscard]] float32 GetRange() const noexcept;
	void SetRange(float32 Range);
	[[nodiscard]] float32 GetInnerConeDegrees() const noexcept;
	[[nodiscard]] float32 GetOuterConeDegrees() const noexcept;
	void SetConeAngles(float32 InnerDegrees, float32 OuterDegrees);
	[[nodiscard]] LightShadowSettings &GetShadowSettings() noexcept;
	[[nodiscard]] const LightShadowSettings &GetShadowSettings() const noexcept;

  private:
	glm::vec3 Color{1.0f};
	float32 LuminousPowerLumens = 2'000.0f;
	float32 Range = 30.0f;
	float32 InnerConeDegrees = 25.0f;
	float32 OuterConeDegrees = 35.0f;
	LightShadowSettings Shadows;
};

class ENGINE_API CObjectDirectionalLightComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<CObjectIdentityComponent, CObjectTransformComponent>;

	explicit CObjectDirectionalLightComponent(world::ObjectHandle Owner) noexcept;
	CCOMPONENT_BODY(CObjectDirectionalLightComponent)

	[[nodiscard]] const glm::vec3 &GetColor() const noexcept;
	void SetColor(const glm::vec3 &LinearColor);
	[[nodiscard]] float32 GetIlluminanceLux() const noexcept;
	void SetIlluminanceLux(float32 Lux);
	[[nodiscard]] float32 GetAngularDiameterDegrees() const noexcept;
	void SetAngularDiameterDegrees(float32 Degrees);
	[[nodiscard]] uint32 GetCascadeCount() const noexcept;
	void SetCascadeCount(uint32 Count);
	[[nodiscard]] float32 GetCascadeDistributionExponent() const noexcept;
	void SetCascadeDistributionExponent(float32 Exponent);
	[[nodiscard]] LightShadowSettings &GetShadowSettings() noexcept;
	[[nodiscard]] const LightShadowSettings &GetShadowSettings() const noexcept;

  private:
	glm::vec3 Color{1.0f};
	float32 IlluminanceLux = 110'000.0f;
	float32 AngularDiameterDegrees = 0.5357f;
	uint32 CascadeCount = 4;
	float32 CascadeDistributionExponent = 2.0f;
	LightShadowSettings Shadows;
};
} // namespace components
