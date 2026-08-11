#include "SkeletonAsset.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace resource
{
namespace
{
constexpr usize MaximumSkeletonJoints = 65'536;

[[nodiscard]] bool IsFinite(const glm::mat4 &Value) noexcept
{
	for (uint32 Column = 0; Column < 4U; ++Column)
		for (uint32 Row = 0; Row < 4U; ++Row)
			if (!std::isfinite(Value[Column][Row]))
				return false;
	return true;
}
} // namespace

SkeletonAsset::SkeletonAsset(string Name, uint64 CompatibilitySignature, std::vector<SkeletonJoint> Joints)
	: Asset(util::UUID::GenerateRandomUUID()), Name(std::move(Name)), CompatibilitySignature(CompatibilitySignature),
	  Joints(std::move(Joints))
{
	if (this->Name.empty() || this->CompatibilitySignature == 0 || this->Joints.empty())
	{
		throw std::invalid_argument("Skeleton requires a name, compatibility signature, and joints");
	}
	if (this->Joints.size() > MaximumSkeletonJoints)
		throw std::length_error("Skeleton exceeds the engine joint budget");
	std::unordered_set<JointID> Ids;
	for (uint32 Index = 0; Index < this->Joints.size(); ++Index)
	{
		const SkeletonJoint &Joint = this->Joints[Index];
		if (Joint.ID == 0 || Joint.Name.empty() || !Ids.insert(Joint.ID).second ||
			(Joint.ParentIndex != InvalidJointIndex && Joint.ParentIndex >= Index) || !IsFinite(Joint.ReferenceLocalTransform) ||
			!IsFinite(Joint.InverseBindMatrix))
		{
			throw std::invalid_argument("Skeleton joints require unique IDs and parent-before-child ordering");
		}
	}
}

const SkeletonJoint *SkeletonAsset::FindJoint(JointID ID) const noexcept
{
	const auto Found = std::find_if(this->Joints.begin(), this->Joints.end(), [ID](const SkeletonJoint &Joint) { return Joint.ID == ID; });
	return Found == this->Joints.end() ? nullptr : &*Found;
}
} // namespace resource
