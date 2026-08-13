#include "CObjectTransformComponent.h"
#include "Source/scene/TransformMath.h"

#include <limits>

static constexpr float32 MaxLookAtUpDot = 0.999f;
static constexpr float32 FloatInfinity = std::numeric_limits<float32>::infinity();
static constexpr glm::vec3 DefaultPosition = glm::vec3{0.0f, 0.0f, 0.0f};
static constexpr glm::quat DefaultRotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
static constexpr glm::vec3 DefaultScale = glm::vec3{1.0f, 1.0f, 1.0f};
static constexpr glm::vec3 LocalForward = glm::vec3{0.0f, 0.0f, -1.0f};
static constexpr glm::vec3 LocalUp = glm::vec3{0.0f, 1.0f, 0.0f};
static constexpr glm::vec3 LocalRight = glm::vec3{1.0f, 0.0f, 0.0f};

glm::vec3 components::CObjectTransformComponent::GetScale() const
{
	return this->Scale;
}

void components::CObjectTransformComponent::LerpScale(const glm::vec3 &Target, float32 Alpha)
{
	if (!world::IsValidTransformScale(Target) || !world::IsFiniteTransformValue(Alpha))
	{
		return;
	}

	const float32 Clamped = glm::clamp(Alpha, 0.0f, 1.0f);

	if (Clamped == 0.0f)
	{
		return;
	}

	const glm::vec3 Result = glm::mix(this->Scale, Target, Clamped);
	if (world::IsValidTransformScale(Result))
	{
		this->Scale = Result;
		this->PublishTransformMutation();
	}
}

void components::CObjectTransformComponent::SetScaleX(float32 X)
{
	if (!world::IsValidTransformScale({X, this->Scale.y, this->Scale.z}))
	{
		return;
	}

	this->Scale.x = X;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetScaleY(float32 Y)
{
	if (!world::IsValidTransformScale({this->Scale.x, Y, this->Scale.z}))
	{
		return;
	}

	this->Scale.y = Y;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetScaleZ(float32 Z)
{
	if (!world::IsValidTransformScale({this->Scale.x, this->Scale.y, Z}))
	{
		return;
	}

	this->Scale.z = Z;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::LookAt(const glm::vec3 &Target, const glm::vec3 &Up)
{
	if (!world::IsFiniteTransformValue(Target) || !world::IsFiniteTransformValue(Up) || !world::IsFiniteTransformValue(this->Position))
	{
		return;
	}

	const glm::vec3 Delta = Target - this->Position;
	glm::vec3 Direction;
	if (!world::TryNormalizeTransformVector(Delta, Direction))
	{
		return;
	}
	glm::vec3 UpNormal;
	if (!world::TryNormalizeTransformVector(Up, UpNormal))
	{
		return;
	}

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

	glm::quat Result;
	if (!world::TryNormalizeTransformQuaternion(glm::quatLookAt(Direction, UpNormal), Result))
	{
		return;
	}
	this->Rotation = Result;
	this->PublishTransformMutation();
}

float32 components::CObjectTransformComponent::DistanceTo(const glm::vec3 &Point) const
{
	if (!world::IsFiniteTransformValue(Point))
	{
		return FloatInfinity;
	}

	const glm::vec3 Delta = this->Position - Point;
	return world::IsFiniteTransformValue(Delta) ? world::StableVectorLength(Delta) : FloatInfinity;
}

float32 components::CObjectTransformComponent::DistanceToSquared(const glm::vec3 &Point) const
{
	if (!world::IsFiniteTransformValue(Point))
	{
		return FloatInfinity;
	}

	const glm::vec3 Delta = this->Position - Point;
	if (!world::IsFiniteTransformValue(Delta))
	{
		return FloatInfinity;
	}
	const float32 Length = world::StableVectorLength(Delta);
	return Length <= std::sqrt(std::numeric_limits<float32>::max()) ? Length * Length : FloatInfinity;
}

bool components::CObjectTransformComponent::IsWithinDistance(const glm::vec3 &Point, float32 Distance) const
{
	if (!world::IsFiniteTransformValue(Distance) || Distance < 0.0f)
	{
		return false;
	}

	return this->DistanceTo(Point) <= Distance;
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

glm::mat4 components::CObjectTransformComponent::GetMatrix() const noexcept
{
	return this->Matrix;
}

uint64 components::CObjectTransformComponent::GetRevision() const noexcept
{
	return this->Revision;
}

void components::CObjectTransformComponent::OnAttachment()
{
}

void components::CObjectTransformComponent::OnDetachment()
{
}

void components::CObjectTransformComponent::PublishTransformMutation()
{
	this->RecalculateMatrix();
	++this->Revision;
	if (this->Revision == 0U)
		this->Revision = 1U;
}

void components::CObjectTransformComponent::RecalculateMatrix()
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
	if (world::IsFiniteTransformValue(Position))
	{
		this->Position = Position;
	}
	else
	{
		this->Position = DefaultPosition;
	}

	glm::quat NormalizedRotation;
	if (world::TryNormalizeTransformQuaternion(Rotation, NormalizedRotation))
	{
		this->Rotation = NormalizedRotation;
	}
	else
	{
		this->Rotation = DefaultRotation;
	}

	if (world::IsValidTransformScale(Scale))
	{
		this->Scale = Scale;
	}
	else
	{
		this->Scale = DefaultScale;
	}

	this->RecalculateMatrix();
}

void components::CObjectTransformComponent::ResetTransform()
{
	this->Position = DefaultPosition;
	this->Rotation = DefaultRotation;
	this->Scale = DefaultScale;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::ResetPosition()
{
	this->Position = DefaultPosition;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::ResetRotation()
{
	this->Rotation = DefaultRotation;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::ResetScale()
{
	this->Scale = DefaultScale;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetTransform(const glm::vec3 &Position, const glm::quat &Rotation, const glm::vec3 &Scale)
{
	if (!world::IsFiniteTransformValue(Position))
	{
		return;
	}
	glm::quat NormalizedRotation;
	if (!world::TryNormalizeTransformQuaternion(Rotation, NormalizedRotation))
	{
		return;
	}
	if (!world::IsValidTransformScale(Scale))
	{
		return;
	}

	this->Position = Position;
	this->Rotation = NormalizedRotation;
	this->Scale = Scale;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetPosition(const glm::vec3 &Position)
{
	if (!world::IsFiniteTransformValue(Position))
	{
		return;
	}

	this->Position = Position;
	this->PublishTransformMutation();
}

glm::vec3 components::CObjectTransformComponent::GetPosition() const
{
	return this->Position;
}

void components::CObjectTransformComponent::Translate(const glm::vec3 &Translation)
{
	if (!world::IsFiniteTransformValue(Translation))
	{
		return;
	}

	const glm::vec3 Result = this->Position + Translation;
	if (!world::IsFiniteTransformValue(Result))
		return;
	this->Position = Result;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::TranslateX(float32 X)
{
	if (!world::IsFiniteTransformValue(X) || !world::IsFiniteTransformValue(this->Position.x + X))
	{
		return;
	}

	this->Position.x += X;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::TranslateY(float32 Y)
{
	if (!world::IsFiniteTransformValue(Y) || !world::IsFiniteTransformValue(this->Position.y + Y))
	{
		return;
	}

	this->Position.y += Y;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::TranslateZ(float32 Z)
{
	if (!world::IsFiniteTransformValue(Z) || !world::IsFiniteTransformValue(this->Position.z + Z))
	{
		return;
	}

	this->Position.z += Z;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::LerpPosition(const glm::vec3 &Target, float32 Alpha)
{
	if (!world::IsFiniteTransformValue(Target) || !world::IsFiniteTransformValue(Alpha))
	{
		return;
	}

	const float32 Clamped = glm::clamp(Alpha, 0.0f, 1.0f);

	if (Clamped == 0.0f)
	{
		return;
	}

	const glm::vec3 Result = glm::mix(this->Position, Target, Clamped);
	if (!world::IsFiniteTransformValue(Result))
		return;
	this->Position = Result;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetPositionX(float32 X)
{
	if (!world::IsFiniteTransformValue(X))
	{
		return;
	}

	this->Position.x = X;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetPositionY(float32 Y)
{
	if (!world::IsFiniteTransformValue(Y))
	{
		return;
	}

	this->Position.y = Y;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetPositionZ(float32 Z)
{
	if (!world::IsFiniteTransformValue(Z))
	{
		return;
	}

	this->Position.z = Z;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetRotation(const glm::quat &Quat)
{
	glm::quat Result;
	if (!world::TryNormalizeTransformQuaternion(Quat, Result))
	{
		return;
	}

	this->Rotation = Result;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetRotationEuler(const glm::vec3 &EulerAngles)
{
	if (!world::IsFiniteTransformValue(EulerAngles))
	{
		return;
	}

	glm::quat Result;
	if (!world::TryNormalizeTransformQuaternion(glm::quat(glm::radians(EulerAngles)), Result))
		return;
	this->Rotation = Result;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::Rotate(float32 AngleDegrees, const glm::vec3 &Axis)
{
	if (!world::IsFiniteTransformValue(AngleDegrees))
	{
		return;
	}

	glm::vec3 NormalAxis;
	if (!world::TryNormalizeTransformVector(Axis, NormalAxis))
		return;
	const glm::quat Delta = glm::angleAxis(glm::radians(AngleDegrees), NormalAxis);
	glm::quat Result;
	if (!world::TryNormalizeTransformQuaternion(Delta * this->Rotation, Result))
		return;
	this->Rotation = Result;
	this->PublishTransformMutation();
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
	if (!world::IsFiniteTransformValue(Alpha))
	{
		return;
	}
	glm::quat SafeTarget;
	if (!world::TryNormalizeTransformQuaternion(Target, SafeTarget))
		return;

	const float32 Clamped = glm::clamp(Alpha, 0.0f, 1.0f);

	if (Clamped == 0.0f)
	{
		return;
	}

	glm::quat Result;
	if (!world::TryNormalizeTransformQuaternion(glm::slerp(this->Rotation, SafeTarget, Clamped), Result))
		return;
	this->Rotation = Result;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetScale(const glm::vec3 &Scale)
{
	if (!world::IsValidTransformScale(Scale))
	{
		return;
	}

	this->Scale = Scale;
	this->PublishTransformMutation();
}

void components::CObjectTransformComponent::SetScale(float32 Scale)
{
	if (!world::IsValidTransformScale(glm::vec3(Scale)))
	{
		return;
	}

	this->Scale = glm::vec3(Scale);
	this->PublishTransformMutation();
}
