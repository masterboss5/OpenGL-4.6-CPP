#include "CObjectTransformComponent.h"

static constexpr float MIN_NORMALIZE_LENGTH = 1e-6f;
static constexpr float MIN_NORMALIZE_LENGTH_SQUARED = MIN_NORMALIZE_LENGTH * MIN_NORMALIZE_LENGTH;
static constexpr float MAX_LOOK_AT_UP_DOT = 0.999f;
static constexpr float FLOAT_INFINITY = std::numeric_limits<float>::infinity();
static constexpr glm::vec3 DEFAULT_POSITION = glm::vec3{0.0f, 0.0f, 0.0f};
static constexpr glm::quat DEFAULT_ROTATION = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
static constexpr glm::vec3 DEFAULT_SCALE = glm::vec3{1.0f, 1.0f, 1.0f};
static constexpr glm::vec3 LOCAL_FORWARD = glm::vec3{0.0f, 0.0f, -1.0f};
static constexpr glm::vec3 LOCAL_UP = glm::vec3{0.0f, 1.0f, 0.0f};
static constexpr glm::vec3 LOCAL_RIGHT = glm::vec3{1.0f, 0.0f, 0.0f};

[[nodiscard]] static __forceinline bool isFinite(float value)
{
	return std::isfinite(value);
}

[[nodiscard]] static __forceinline bool isFinite(const glm::vec3& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] static __forceinline bool isFinite(const glm::quat& value)
{
	return std::isfinite(value.w) && std::isfinite(value.x)
		&& std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] static __forceinline bool isValidScale(const glm::vec3& value)
{
	return isFinite(value) && value.x > 0.0f && value.y > 0.0f
		&& value.z > 0.0f;
}

glm::vec3 components::CObjectTransformComponent::getScale() const
{
	return this->scale;
}

void components::CObjectTransformComponent::lerpScale(const glm::vec3& target, float alpha)
{
	if (!isValidScale(target) || !isFinite(alpha))
	{
		return;
	}

	const float clamped = glm::clamp(alpha, 0.0f, 1.0f);

	if (clamped == 0.0f)
	{
		return;
	}

	this->scale = glm::mix(this->scale, target, clamped);
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setScaleX(float x)
{
	if (!isFinite(x) || x <= 0.0f)
	{
		return;
	}

	this->scale.x = x;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setScaleY(float y)
{
	if (!isFinite(y) || y <= 0.0f)
	{
		return;
	}

	this->scale.y = y;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setScaleZ(float z)
{
	if (!isFinite(z) || z <= 0.0f)
	{
		return;
	}

	this->scale.z = z;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::lookAt(const glm::vec3& target, const glm::vec3& up)
{
	if (!isFinite(target) || !isFinite(up) || !isFinite(this->position))
	{
		return;
	}

	const glm::vec3 delta = target - this->position;

	if (glm::dot(delta, delta) <= MIN_NORMALIZE_LENGTH_SQUARED)
	{
		return;
	}

	if (glm::dot(up, up) <= MIN_NORMALIZE_LENGTH_SQUARED)
	{
		return;
	}

	const glm::vec3 direction = glm::normalize(delta);
	glm::vec3 upNormal = glm::normalize(up);

	if (std::abs(glm::dot(direction, upNormal)) > MAX_LOOK_AT_UP_DOT)
	{
		if (std::abs(direction.y) < MAX_LOOK_AT_UP_DOT)
		{
			upNormal = glm::vec3{0.0f, 1.0f, 0.0f};
		}
		else
		{
			upNormal = glm::vec3{1.0f, 0.0f, 0.0f};
		}
	}

	this->rotation = glm::normalize(glm::quatLookAt(direction, upNormal));
	this->needsRecalculation = true;
}

float components::CObjectTransformComponent::distanceTo(const glm::vec3& point) const
{
	if (!isFinite(point))
	{
		return FLOAT_INFINITY;
	}

	return glm::distance(this->position, point);
}

float components::CObjectTransformComponent::distanceToSquared(const glm::vec3& point) const
{
	if (!isFinite(point))
	{
		return FLOAT_INFINITY;
	}

	const glm::vec3 delta = this->position - point;
	return glm::dot(delta, delta);
}

bool components::CObjectTransformComponent::isWithinDistance(const glm::vec3& point, float distance) const
{
	if (!isFinite(distance) || distance < 0.0f)
	{
		return false;
	}

	const float distanceSq = this->distanceToSquared(point);
	const float maxDistanceSq = distance * distance;

	if (!isFinite(distanceSq) || !isFinite(maxDistanceSq))
	{
		return false;
	}

	if (distanceSq > maxDistanceSq)
	{
		return false;
	}

	return true;
}

glm::vec3 components::CObjectTransformComponent::getForward() const
{
	return this->rotation * LOCAL_FORWARD;
}

glm::vec3 components::CObjectTransformComponent::getUp() const
{
	return this->rotation * LOCAL_UP;
}

glm::vec3 components::CObjectTransformComponent::getRight() const
{
	return this->rotation * LOCAL_RIGHT;
}

const glm::mat4& components::CObjectTransformComponent::getMatrix() const
{
	this->updateMatrix();
	return this->matrix;
}

void components::CObjectTransformComponent::onAttachment()
{
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::onDetachment()
{
}

void components::CObjectTransformComponent::onRender()
{
}

void components::CObjectTransformComponent::onUpdate()
{
	//this->updateMatrix(); Maybe add back later?
}

void components::CObjectTransformComponent::updateMatrix() const
{
	if (this->needsRecalculation)
	{
		this->recalculateMatrix();
		this->needsRecalculation = false;
	}
}

void components::CObjectTransformComponent::recalculateMatrix() const
{
	const float qx = this->rotation.x, qy = this->rotation.y;
	const float qz = this->rotation.z, qw = this->rotation.w;

	const float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
	const float xx = qx * x2, xy = qx * y2, xz = qx * z2;
	const float yy = qy * y2, yz = qy * z2, zz = qz * z2;
	const float wx = qw * x2, wy = qw * y2, wz = qw * z2;

	const float sx = this->scale.x, sy = this->scale.y, sz = this->scale.z;

	this->matrix = glm::mat4(
		glm::vec4((1.0f - (yy + zz)) * sx, (xy + wz) * sx, (xz - wy) * sx, 0.0f),
		glm::vec4((xy - wz) * sy, (1.0f - (xx + zz)) * sy, (yz + wx) * sy, 0.0f),
		glm::vec4((xz + wy) * sz, (yz - wx) * sz, (1.0f - (xx + yy)) * sz, 0.0f),
		glm::vec4(this->position.x, this->position.y, this->position.z, 1.0f)
	);
}

components::CObjectTransformComponent::CObjectTransformComponent
(
	world::Object* object,
	const glm::vec3& position,
	const glm::quat& rotation,
	const glm::vec3& scale
)
	: CObjectComponent(object)
{
	if (isFinite(position))
	{
		this->position = position;
	}
	else
	{
		this->position = DEFAULT_POSITION;
	}

	if (isFinite(rotation) && glm::dot(rotation, rotation) > MIN_NORMALIZE_LENGTH_SQUARED)
	{
		this->rotation = glm::normalize(rotation);
	}
	else
	{
		this->rotation = DEFAULT_ROTATION;
	}

	if (isValidScale(scale))
	{
		this->scale = scale;
	}
	else
	{
		this->scale = DEFAULT_SCALE;
	}

	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::resetTransform()
{
	this->position = DEFAULT_POSITION;
	this->rotation = DEFAULT_ROTATION;
	this->scale = DEFAULT_SCALE;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::resetPosition()
{
	this->position = DEFAULT_POSITION;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::resetRotation()
{
	this->rotation = DEFAULT_ROTATION;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::resetScale()
{
	this->scale = DEFAULT_SCALE;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setTransform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
{
	if (!isFinite(position))
	{
		return;
	}

	if (!isFinite(rotation) || glm::dot(rotation, rotation) <= MIN_NORMALIZE_LENGTH_SQUARED)
	{
		return;
	}

	if (!isValidScale(scale))
	{
		return;
	}

	this->position = position;
	this->rotation = glm::normalize(rotation);
	this->scale = scale;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setPosition(const glm::vec3& position)
{
	if (!isFinite(position))
	{
		return;
	}

	this->position = position;
	this->needsRecalculation = true;
}

glm::vec3 components::CObjectTransformComponent::getPosition() const
{
	return this->position;
}

void components::CObjectTransformComponent::translate(const glm::vec3& translation)
{
	if (!isFinite(translation))
	{
		return;
	}

	this->position += translation;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::translateX(float x)
{
	if (!isFinite(x))
	{
		return;
	}

	this->position.x += x;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::translateY(float y)
{
	if (!isFinite(y))
	{
		return;
	}

	this->position.y += y;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::translateZ(float z)
{
	if (!isFinite(z))
	{
		return;
	}

	this->position.z += z;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::lerpPosition(const glm::vec3& target, float alpha)
{
	if (!isFinite(target) || !isFinite(alpha))
	{
		return;
	}

	const float clamped = glm::clamp(alpha, 0.0f, 1.0f);

	if (clamped == 0.0f)
	{
		return;
	}

	this->position = glm::mix(this->position, target, clamped);
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setPositionX(float x)
{
	if (!isFinite(x))
	{
		return;
	}

	this->position.x = x;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setPositionY(float y)
{
	if (!isFinite(y))
	{
		return;
	}

	this->position.y = y;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setPositionZ(float z)
{
	if (!isFinite(z))
	{
		return;
	}

	this->position.z = z;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setRotation(const glm::quat& quat)
{
	if (!isFinite(quat) || glm::dot(quat, quat) <= MIN_NORMALIZE_LENGTH_SQUARED)
	{
		return;
	}

	this->rotation = glm::normalize(quat);
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setRotationEuler(const glm::vec3& eulerAngles)
{
	if (!isFinite(eulerAngles))
	{
		return;
	}

	this->rotation = glm::quat(glm::radians(eulerAngles));
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::rotate(float angleDegrees, const glm::vec3& axis)
{
	if (!isFinite(angleDegrees) || !isFinite(axis) || glm::dot(axis, axis) <= MIN_NORMALIZE_LENGTH_SQUARED)
	{
		return;
	}

	const glm::vec3 normalAxis = glm::normalize(axis);
	const glm::quat delta = glm::angleAxis(glm::radians(angleDegrees), normalAxis);
	this->rotation = glm::normalize(delta * this->rotation);
	this->needsRecalculation = true;
}

glm::quat components::CObjectTransformComponent::getRotation() const
{
	return this->rotation;
}

glm::vec3 components::CObjectTransformComponent::getRotationEuler() const
{
	return glm::degrees(glm::eulerAngles(this->rotation));
}

void components::CObjectTransformComponent::slerpRotation(const glm::quat& target, float alpha)
{
	if (!isFinite(alpha) || !isFinite(target) || glm::dot(target, target) <= MIN_NORMALIZE_LENGTH_SQUARED)
	{
		return;
	}

	const float clamped = glm::clamp(alpha, 0.0f, 1.0f);

	if (clamped == 0.0f)
	{
		return;
	}

	const glm::quat safeTarget = glm::normalize(target);
	this->rotation = glm::normalize(glm::slerp(this->rotation, safeTarget, clamped));
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setScale(const glm::vec3& scale)
{
	if (!isValidScale(scale))
	{
		return;
	}

	this->scale = scale;
	this->needsRecalculation = true;
}

void components::CObjectTransformComponent::setScale(float scale)
{
	if (!isFinite(scale) || scale <= 0.0f)
	{
		return;
	}

	this->scale = glm::vec3(scale);
	this->needsRecalculation = true;
}