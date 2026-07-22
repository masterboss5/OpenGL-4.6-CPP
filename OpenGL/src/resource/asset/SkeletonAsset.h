#pragma once

#include "src/resource/Asset.h"
#include "src/resource/asset/AssetTypes.h"

#include <glm.hpp>
#include <limits>
#include <span>
#include <vector>

namespace resource
{
using JointID = uint64;
inline constexpr uint32 InvalidJointIndex = std::numeric_limits<uint32>::max();

struct SkeletonJoint final
{
	JointID ID = 0;
	string Name;
	uint32 ParentIndex = InvalidJointIndex;
	glm::mat4 ReferenceLocalTransform{1.0f};
	glm::mat4 InverseBindMatrix{1.0f};
};

class SkeletonAsset final : public Asset
{
  public:
	inline static constexpr resource::AssetType AssetType = resource::AssetType::Skeleton;
	SkeletonAsset(string Name, uint64 CompatibilitySignature, std::vector<SkeletonJoint> Joints);

	[[nodiscard]] string_view GetName() const noexcept
	{
		return this->Name;
	}
	[[nodiscard]] uint64 GetCompatibilitySignature() const noexcept
	{
		return this->CompatibilitySignature;
	}
	[[nodiscard]] std::span<const SkeletonJoint> GetJoints() const noexcept
	{
		return this->Joints;
	}
	[[nodiscard]] const SkeletonJoint *FindJoint(JointID ID) const noexcept;

  private:
	string Name;
	uint64 CompatibilitySignature = 0;
	std::vector<SkeletonJoint> Joints;
};
} // namespace resource
