#include "SkeletonAsset.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace resource
{
SkeletonAsset::SkeletonAsset(string Name, uint64 CompatibilitySignature, std::vector<SkeletonJoint> Joints)
	: Asset(util::UUID::GenerateRandomUUID()), Name(std::move(Name)), CompatibilitySignature(CompatibilitySignature),
	  Joints(std::move(Joints))
{
	if (this->Name.empty() || this->CompatibilitySignature == 0 || this->Joints.empty())
	{
		throw std::invalid_argument("Skeleton requires a name, compatibility signature, and joints");
	}
	std::unordered_set<JointID> Ids;
	for (uint32 Index = 0; Index < this->Joints.size(); ++Index)
	{
		const SkeletonJoint &Joint = this->Joints[Index];
		if (Joint.ID == 0 || Joint.Name.empty() || !Ids.insert(Joint.ID).second ||
			(Joint.ParentIndex != InvalidJointIndex && Joint.ParentIndex >= Index))
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
