#include "CObjectTransformComponent.h"

static constexpr float32 MinNormalizeLength = 1e-6f;
static constexpr float32 MinNormalizeLengthSquared = MinNormalizeLength * MinNormalizeLength;
static constexpr float32 MaxLookAtUpDot = 0.999f;
static constexpr float32 FloatInfinity = std::numeric_limits<float32>::infinity();
static constexpr glm::vec3 DefaultPosition = glm::vec3{0.0f, 0.0f, 0.0f};
static constexpr glm::quat DefaultRotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
static constexpr glm::vec3 DefaultScale = glm::vec3{1.0f, 1.0f, 1.0f};
static constexpr glm::vec3 LocalForward = glm::vec3{0.0f, 0.0f, -1.0f};
static constexpr glm::vec3 LocalUp = glm::vec3{0.0f, 1.0f, 0.0f};
static constexpr glm::vec3 LocalRight = glm::vec3{1.0f, 0.0f, 0.0f};

[[nodiscard]] static __forceinline bool IsFinite(float32 Value)
{
	return std::isfinite(Value);
}

[[nodiscard]] static __forceinline bool IsFinite(const glm::vec3 &Value)
{
	return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

[[nodiscard]] static __forceinline bool IsFinite(const glm::quat &Value)
{
	return std::isfinite(Value.w) && std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

[[nodiscard]] static __forceinline bool IsValidScale(const glm::vec3 &Value)
{
	return IsFinite(Value) && Value.x > 0.0f && Value.y > 0.0f && Value.z > 0.0f;
}

glm::vec3 components::CObjectTransformComponent::GetScale() const
{
	return this->Scale;
}

void components::CObjectTransformComponent::LerpScale(const glm::vec3 &Target, float32 Alpha)
{
	if (!IsValidScale(Target) || !IsFinite(Alpha))
	{
		return;
	}

	const float32 Clamped = glm::clamp(Alpha, 0.0f, 1.0f);

	if (Clamped == 0.0f)
	{
		return;
	}

	this->Scale = glm::mix(this->Scale, Target, Clamped);
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetScaleX(float32 X)
{
	if (!IsFinite(X) || X <= 0.0f)
	{
		return;
	}

	this->Scale.x = X;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetScaleY(float32 Y)
{
	if (!IsFinite(Y) || Y <= 0.0f)
	{
		return;
	}

	this->Scale.y = Y;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetScaleZ(float32 Z)
{
	if (!IsFinite(Z) || Z <= 0.0f)
	{
		return;
	}

	this->Scale.z = Z;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::LookAt(const glm::vec3 &Target, const glm::vec3 &Up)
{
	if (!IsFinite(Target) || !IsFinite(Up) || !IsFinite(this->Position))
	{
		return;
	}

	const glm::vec3 Delta = Target - this->Position;

	if (glm::dot(Delta, Delta) <= MinNormalizeLengthSquared)
	{
		return;
	}

	if (glm::dot(Up, Up) <= MinNormalizeLengthSquared)
	{
		return;
	}

	const glm::vec3 Direction = glm::normalize(Delta);
	glm::vec3 UpNormal = glm::normalize(Up);

	if (std::abs(glm::dot(Direction, UpNormal)) > MaxLookAtUpDot)
	{
		if (std::abs(Direction.y) < MaxLookAtUpDot)
		{
			UpNormal = glm::vec3{0.0f, 1.0f, 0.0f};
		}
		else
		{
			UpNormal = glm::vec3{1.0f, 0.0f, 0.0f};
		}
	}

	this->Rotation = glm::normalize(glm::quatLookAt(Direction, UpNormal));
	this->NeedsRecalculation = true;
}

float32 components::CObjectTransformComponent::DistanceTo(const glm::vec3 &Point) const
{
	if (!IsFinite(Point))
	{
		return FloatInfinity;
	}

	return glm::distance(this->Position, Point);
}

float32 components::CObjectTransformComponent::DistanceToSquared(const glm::vec3 &Point) const
{
	if (!IsFinite(Point))
	{
		return FloatInfinity;
	}

	const glm::vec3 Delta = this->Position - Point;
	return glm::dot(Delta, Delta);
}

bool components::CObjectTransformComponent::IsWithinDistance(const glm::vec3 &Point, float32 Distance) const
{
	if (!IsFinite(Distance) || Distance < 0.0f)
	{
		return false;
	}

	const float32 DistanceSq = this->DistanceToSquared(Point);
	const float32 MaxDistanceSq = Distance * Distance;

	if (!IsFinite(DistanceSq) || !IsFinite(MaxDistanceSq))
	{
		return false;
	}

	if (DistanceSq > MaxDistanceSq)
	{
		return false;
	}

	return true;
}

glm::vec3 components::CObjectTransformComponent::GetForward() const
{
	return this->Rotation * LocalForward;
}

glm::vec3 components::CObjectTransformComponent::GetUp() const
{
	return this->Rotation * LocalUp;
}

glm::vec3 components::CObjectTransformComponent::GetRight() const
{
	return this->Rotation * LocalRight;
}

const glm::mat4 &components::CObjectTransformComponent::GetMatrix() const
{
	this->UpdateMatrix();
	return this->Matrix;
}

void components::CObjectTransformComponent::OnAttachment()
{
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::OnDetachment()
{
}

void components::CObjectTransformComponent::UpdateMatrix() const
{
	if (this->NeedsRecalculation)
	{
		this->RecalculateMatrix();
		this->NeedsRecalculation = false;
	}
}

void components::CObjectTransformComponent::RecalculateMatrix() const
{
	const float32 Qx = this->Rotation.x, Qy = this->Rotation.y;
	const float32 Qz = this->Rotation.z, Qw = this->Rotation.w;
	const float32 X2 = Qx + Qx, Y2 = Qy + Qy, Z2 = Qz + Qz;
	const float32 Xx = Qx * X2, Xy = Qx * Y2, Xz = Qx * Z2;
	const float32 Yy = Qy * Y2, Yz = Qy * Z2, Zz = Qz * Z2;
	const float32 Wx = Qw * X2, Wy = Qw * Y2, Wz = Qw * Z2;
	const float32 Sx = this->Scale.x, Sy = this->Scale.y, Sz = this->Scale.z;

	this->Matrix = glm::mat4(glm::vec4((1.0f - (Yy + Zz)) * Sx, (Xy + Wz) * Sx, (Xz - Wy) * Sx, 0.0f),
							 glm::vec4((Xy - Wz) * Sy, (1.0f - (Xx + Zz)) * Sy, (Yz + Wx) * Sy, 0.0f),
							 glm::vec4((Xz + Wy) * Sz, (Yz - Wx) * Sz, (1.0f - (Xx + Yy)) * Sz, 0.0f),
							 glm::vec4(this->Position.x, this->Position.y, this->Position.z, 1.0f));
}

components::CObjectTransformComponent::CObjectTransformComponent(world::ObjectHandle Owner, const glm::vec3 &Position,
																 const glm::quat &Rotation, const glm::vec3 &Scale)
	: CObjectComponent(Owner)
{
	if (IsFinite(Position))
	{
		this->Position = Position;
	}
	else
	{
		this->Position = DefaultPosition;
	}

	if (IsFinite(Rotation) && glm::dot(Rotation, Rotation) > MinNormalizeLengthSquared)
	{
		this->Rotation = glm::normalize(Rotation);
	}
	else
	{
		this->Rotation = DefaultRotation;
	}

	if (IsValidScale(Scale))
	{
		this->Scale = Scale;
	}
	else
	{
		this->Scale = DefaultScale;
	}

	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::ResetTransform()
{
	this->Position = DefaultPosition;
	this->Rotation = DefaultRotation;
	this->Scale = DefaultScale;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::ResetPosition()
{
	this->Position = DefaultPosition;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::ResetRotation()
{
	this->Rotation = DefaultRotation;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::ResetScale()
{
	this->Scale = DefaultScale;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetTransform(const glm::vec3 &Position, const glm::quat &Rotation, const glm::vec3 &Scale)
{
	if (!IsFinite(Position))
	{
		return;
	}
	if (!IsFinite(Rotation) || glm::dot(Rotation, Rotation) <= MinNormalizeLengthSquared)
	{
		return;
	}
	if (!IsValidScale(Scale))
	{
		return;
	}

	this->Position = Position;
	this->Rotation = glm::normalize(Rotation);
	this->Scale = Scale;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetPosition(const glm::vec3 &Position)
{
	if (!IsFinite(Position))
	{
		return;
	}

	this->Position = Position;
	this->NeedsRecalculation = true;
}

glm::vec3 components::CObjectTransformComponent::GetPosition() const
{
	return this->Position;
}

void components::CObjectTransformComponent::Translate(const glm::vec3 &Translation)
{
	if (!IsFinite(Translation))
	{
		return;
	}

	this->Position += Translation;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::TranslateX(float32 X)
{
	if (!IsFinite(X))
	{
		return;
	}

	this->Position.x += X;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::TranslateY(float32 Y)
{
	if (!IsFinite(Y))
	{
		return;
	}

	this->Position.y += Y;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::TranslateZ(float32 Z)
{
	if (!IsFinite(Z))
	{
		return;
	}

	this->Position.z += Z;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::LerpPosition(const glm::vec3 &Target, float32 Alpha)
{
	if (!IsFinite(Target) || !IsFinite(Alpha))
	{
		return;
	}

	const float32 Clamped = glm::clamp(Alpha, 0.0f, 1.0f);

	if (Clamped == 0.0f)
	{
		return;
	}

	this->Position = glm::mix(this->Position, Target, Clamped);
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetPositionX(float32 X)
{
	if (!IsFinite(X))
	{
		return;
	}

	this->Position.x = X;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetPositionY(float32 Y)
{
	if (!IsFinite(Y))
	{
		return;
	}

	this->Position.y = Y;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetPositionZ(float32 Z)
{
	if (!IsFinite(Z))
	{
		return;
	}

	this->Position.z = Z;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetRotation(const glm::quat &Quat)
{
	if (!IsFinite(Quat) || glm::dot(Quat, Quat) <= MinNormalizeLengthSquared)
	{
		return;
	}

	this->Rotation = glm::normalize(Quat);
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetRotationEuler(const glm::vec3 &EulerAngles)
{
	if (!IsFinite(EulerAngles))
	{
		return;
	}

	this->Rotation = glm::quat(glm::radians(EulerAngles));
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::Rotate(float32 AngleDegrees, const glm::vec3 &Axis)
{
	if (!IsFinite(AngleDegrees) || !IsFinite(Axis) || glm::dot(Axis, Axis) <= MinNormalizeLengthSquared)
	{
		return;
	}

	const glm::vec3 NormalAxis = glm::normalize(Axis);
	const glm::quat Delta = glm::angleAxis(glm::radians(AngleDegrees), NormalAxis);
	this->Rotation = glm::normalize(Delta * this->Rotation);
	this->NeedsRecalculation = true;
}

glm::quat components::CObjectTransformComponent::GetRotation() const
{
	return this->Rotation;
}

glm::vec3 components::CObjectTransformComponent::GetRotationEuler() const
{
	return glm::degrees(glm::eulerAngles(this->Rotation));
}

void components::CObjectTransformComponent::SlerpRotation(const glm::quat &Target, float32 Alpha)
{
	if (!IsFinite(Alpha) || !IsFinite(Target) || glm::dot(Target, Target) <= MinNormalizeLengthSquared)
	{
		return;
	}

	const float32 Clamped = glm::clamp(Alpha, 0.0f, 1.0f);

	if (Clamped == 0.0f)
	{
		return;
	}

	const glm::quat SafeTarget = glm::normalize(Target);
	this->Rotation = glm::normalize(glm::slerp(this->Rotation, SafeTarget, Clamped));
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetScale(const glm::vec3 &Scale)
{
	if (!IsValidScale(Scale))
	{
		return;
	}

	this->Scale = Scale;
	this->NeedsRecalculation = true;
}

void components::CObjectTransformComponent::SetScale(float32 Scale)
{
	if (!IsFinite(Scale) || Scale <= 0.0f)
	{
		return;
	}

	this->Scale = glm::vec3(Scale);
	this->NeedsRecalculation = true;
}
