#pragma once

#include "src/types.h"

#include <glm.hpp>
#include <gtc/quaternion.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace world
{
inline constexpr float32 TransformComparisonEpsilon = 1.0e-4f;
inline constexpr float32 MinimumTransformScale = TransformComparisonEpsilon;
inline constexpr float32 MaximumTransformScale = std::numeric_limits<float32>::max() / 4.0f;

[[nodiscard]] inline bool IsFiniteTransformValue(const float32 Value) noexcept
{
	return std::isfinite(Value);
}

template <glm::length_t Length, typename ValueType, glm::qualifier Qualifier>
[[nodiscard]] inline bool IsFiniteTransformValue(const glm::vec<Length, ValueType, Qualifier> &Value) noexcept
{
	for (glm::length_t Index = 0; Index < Length; ++Index)
	{
		if (!std::isfinite(Value[Index]))
		{
			return false;
		}
	}
	return true;
}

[[nodiscard]] inline bool IsFiniteTransformValue(const glm::quat &Value) noexcept
{
	return std::isfinite(Value.w) && std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

[[nodiscard]] inline bool IsFiniteTransformValue(const glm::mat4 &Value) noexcept
{
	return IsFiniteTransformValue(Value[0]) && IsFiniteTransformValue(Value[1]) && IsFiniteTransformValue(Value[2]) &&
		   IsFiniteTransformValue(Value[3]);
}

[[nodiscard]] inline float32 StableVectorLength(const glm::vec3 &Value) noexcept
{
	const glm::vec3 Absolute = glm::abs(Value);
	const float32 Maximum = glm::max(Absolute.x, glm::max(Absolute.y, Absolute.z));
	if (Maximum == 0.0f || !std::isfinite(Maximum))
	{
		return Maximum;
	}
	return Maximum * glm::length(Value / Maximum);
}

[[nodiscard]] inline bool TryNormalizeTransformVector(const glm::vec3 &Value, glm::vec3 &Normalized,
													  const float32 MinimumLength = TransformComparisonEpsilon) noexcept
{
	if (!IsFiniteTransformValue(Value))
	{
		return false;
	}
	const float32 Length = StableVectorLength(Value);
	if (!std::isfinite(Length) || Length <= MinimumLength)
	{
		return false;
	}
	Normalized = Value / Length;
	return IsFiniteTransformValue(Normalized);
}

[[nodiscard]] inline bool TryNormalizeTransformQuaternion(const glm::quat &Value, glm::quat &Normalized) noexcept
{
	if (!IsFiniteTransformValue(Value))
	{
		return false;
	}
	const float32 Maximum = glm::max(glm::max(std::abs(Value.w), std::abs(Value.x)), glm::max(std::abs(Value.y), std::abs(Value.z)));
	if (Maximum == 0.0f || !std::isfinite(Maximum))
	{
		return false;
	}
	const glm::quat Scaled = Value / Maximum;
	const float32 Length = std::sqrt(glm::dot(Scaled, Scaled));
	if (!std::isfinite(Length) || Length <= std::numeric_limits<float32>::epsilon())
	{
		return false;
	}
	Normalized = Scaled / Length;
	return IsFiniteTransformValue(Normalized);
}

[[nodiscard]] inline bool IsValidTransformScale(const glm::vec3 &Value) noexcept
{
	return IsFiniteTransformValue(Value) && Value.x > MinimumTransformScale && Value.y > MinimumTransformScale &&
		   Value.z > MinimumTransformScale && Value.x <= MaximumTransformScale && Value.y <= MaximumTransformScale &&
		   Value.z <= MaximumTransformScale;
}

struct DecomposedTransform final
{
	glm::vec3 Position{0.0f};
	glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 Scale{1.0f};
};

[[nodiscard]] inline DecomposedTransform DecomposeAffineTransform(const glm::mat4 &Matrix)
{
	if (!IsFiniteTransformValue(Matrix))
	{
		throw std::invalid_argument("Transform decomposition requires finite matrix coefficients");
	}
	if (std::abs(Matrix[0][3]) > TransformComparisonEpsilon || std::abs(Matrix[1][3]) > TransformComparisonEpsilon ||
		std::abs(Matrix[2][3]) > TransformComparisonEpsilon || std::abs(Matrix[3][3] - 1.0f) > TransformComparisonEpsilon)
	{
		throw std::invalid_argument("Transform decomposition does not support perspective");
	}

	DecomposedTransform Result{.Position = glm::vec3(Matrix[3])};
	glm::vec3 AxisX(Matrix[0]);
	glm::vec3 AxisY(Matrix[1]);
	glm::vec3 AxisZ(Matrix[2]);
	Result.Scale = {StableVectorLength(AxisX), StableVectorLength(AxisY), StableVectorLength(AxisZ)};
	if (!IsValidTransformScale(Result.Scale))
		throw std::invalid_argument("Transform decomposition encountered a degenerate scale");
	AxisX /= Result.Scale.x;
	AxisY /= Result.Scale.y;
	AxisZ /= Result.Scale.z;
	if (!IsFiniteTransformValue(AxisX) || !IsFiniteTransformValue(AxisY) || !IsFiniteTransformValue(AxisZ) ||
		std::abs(glm::dot(AxisX, AxisY)) > TransformComparisonEpsilon || std::abs(glm::dot(AxisX, AxisZ)) > TransformComparisonEpsilon ||
		std::abs(glm::dot(AxisY, AxisZ)) > TransformComparisonEpsilon)
	{
		throw std::invalid_argument("Transform decomposition would require shear");
	}
	if (glm::dot(glm::cross(AxisX, AxisY), AxisZ) <= 0.0f)
		throw std::invalid_argument("Transform decomposition encountered a reflected or degenerate basis");
	const glm::quat Rotation = glm::quat_cast(glm::mat3(AxisX, AxisY, AxisZ));
	if (!TryNormalizeTransformQuaternion(Rotation, Result.Rotation) || !IsFiniteTransformValue(Result.Position))
	{
		throw std::invalid_argument("Transform decomposition produced non-finite state");
	}
	return Result;
}
} // namespace world
