#include "AnimationAsset.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace resource
{
AnimationClipAsset::AnimationClipAsset(string Name, AssetHandle<SkeletonAsset> Skeleton, float32 Duration, float32 SampleRate,
									   std::vector<AnimationJointTrack> JointTracks, std::vector<AnimationMorphTrack> MorphTracks,
									   std::vector<AnimationEvent> Events, bool ContainsRootMotion)
	: Asset(util::UUID::GenerateRandomUUID()), Name(std::move(Name)), Skeleton(std::move(Skeleton)), Duration(Duration),
	  SampleRate(SampleRate), JointTracks(std::move(JointTracks)), MorphTracks(std::move(MorphTracks)), Events(std::move(Events)),
	  ContainsRootMotion(ContainsRootMotion)
{
	if (this->Name.empty() || !std::isfinite(this->Duration) || this->Duration <= 0.0f || !std::isfinite(this->SampleRate) ||
		this->SampleRate <= 0.0f)
	{
		throw std::invalid_argument("Animation clip requires a name, positive duration, and positive sample rate");
	}
	if ((!this->JointTracks.empty() || this->ContainsRootMotion) && !this->Skeleton)
		throw std::invalid_argument("Joint animation and root motion require a skeleton");
	if (this->JointTracks.empty() && this->MorphTracks.empty())
		throw std::invalid_argument("Animation clip requires at least one joint or morph track");
	std::unordered_set<JointID> AnimatedJoints;
	std::unordered_set<AnimationTrackID> TrackIDs;
	AssetPtr<SkeletonAsset> PinnedSkeleton;
	if (this->Skeleton)
		PinnedSkeleton = this->Skeleton.Pin();
	for (const AnimationJointTrack &Track : this->JointTracks)
	{
		if (Track.ID == 0 || Track.Joint == 0 || Track.Keys.empty() || !TrackIDs.insert(Track.ID).second ||
			!AnimatedJoints.insert(Track.Joint).second || PinnedSkeleton == nullptr || PinnedSkeleton->FindJoint(Track.Joint) == nullptr)
		{
			throw std::invalid_argument("Animation clip joint tracks are invalid or duplicated");
		}
		float32 PreviousTime = -1.0f;
		for (const AnimationTransformKey &Key : Track.Keys)
		{
			const bool FiniteTransform = std::isfinite(Key.Translation.x) && std::isfinite(Key.Translation.y) &&
										 std::isfinite(Key.Translation.z) && std::isfinite(Key.Rotation.w) &&
										 std::isfinite(Key.Rotation.x) && std::isfinite(Key.Rotation.y) && std::isfinite(Key.Rotation.z) &&
										 std::isfinite(Key.Scale.x) && std::isfinite(Key.Scale.y) && std::isfinite(Key.Scale.z);
			if (!std::isfinite(Key.Time) || Key.Time < PreviousTime || Key.Time < 0.0f || Key.Time > this->Duration || !FiniteTransform ||
				glm::length(Key.Rotation) <= std::numeric_limits<float32>::epsilon())
			{
				throw std::invalid_argument("Animation clip keys must be ordered inside the clip duration");
			}
			PreviousTime = Key.Time;
		}
	}
	std::unordered_set<uint64> AnimatedMorphTargets;
	for (const AnimationMorphTrack &Track : this->MorphTracks)
	{
		if (Track.MorphTarget == 0 || Track.Keys.empty() || !AnimatedMorphTargets.insert(Track.MorphTarget).second)
			throw std::invalid_argument("Animation morph tracks require unique stable targets and keys");
		float32 PreviousTime = -1.0f;
		for (const AnimationMorphKey &Key : Track.Keys)
		{
			if (!std::isfinite(Key.Time) || !std::isfinite(Key.Weight) || Key.Time < PreviousTime || Key.Time < 0.0f ||
				Key.Time > this->Duration)
				throw std::invalid_argument("Animation morph keys must be finite and ordered inside the clip duration");
			PreviousTime = Key.Time;
		}
	}
	std::unordered_set<uint64> EventIDs;
	float32 PreviousEventTime = -1.0f;
	for (const AnimationEvent &Event : this->Events)
	{
		if (Event.ID == 0 || Event.Name.empty() || !EventIDs.insert(Event.ID).second || !std::isfinite(Event.Time) ||
			Event.Time < PreviousEventTime || Event.Time < 0.0f || Event.Time > this->Duration)
			throw std::invalid_argument("Animation events require unique IDs and ordered times inside the clip duration");
		PreviousEventTime = Event.Time;
	}
}

AnimationGraphAsset::AnimationGraphAsset(string Name, std::vector<AnimationParameterDefinition> Parameters,
										 std::vector<AnimationGraphNode> Nodes, AnimationNodeID OutputNode)
	: Asset(util::UUID::GenerateRandomUUID()), Name(std::move(Name)), Parameters(std::move(Parameters)), Nodes(std::move(Nodes)),
	  OutputNode(OutputNode)
{
	if (this->Name.empty() || this->Nodes.empty() || this->OutputNode == 0)
	{
		throw std::invalid_argument("Animation graph requires a name, nodes, and output node");
	}
	std::unordered_set<AnimationParameterID> ParameterIDs;
	std::unordered_map<AnimationParameterID, AnimationParameterType> ParameterTypes;
	for (const auto &Parameter : this->Parameters)
	{
		if (Parameter.ID == 0 || Parameter.Name.empty() || !ParameterIDs.insert(Parameter.ID).second ||
			!std::isfinite(Parameter.DefaultValue.x) || !std::isfinite(Parameter.DefaultValue.y) ||
			!std::isfinite(Parameter.DefaultValue.z) || !std::isfinite(Parameter.DefaultValue.w))
		{
			throw std::invalid_argument("Animation graph parameters require unique IDs and names");
		}
		ParameterTypes.emplace(Parameter.ID, Parameter.Type);
	}
	std::unordered_map<AnimationNodeID, const AnimationGraphNode *> NodeMap;
	for (const AnimationGraphNode &Node : this->Nodes)
	{
		if (Node.ID == 0 || !NodeMap.emplace(Node.ID, &Node).second)
		{
			throw std::invalid_argument("Animation graph node IDs must be unique");
		}
		if (Node.Type == AnimationGraphNodeType::Clip && !Node.Clip)
		{
			throw std::invalid_argument("Animation graph clip node requires a clip handle");
		}
	}
	if (NodeMap.find(this->OutputNode) == NodeMap.end() || NodeMap[this->OutputNode]->Type != AnimationGraphNodeType::Output)
	{
		throw std::invalid_argument("Animation graph output must reference an Output node");
	}
	for (const AnimationGraphNode &Node : this->Nodes)
	{
		if (Node.Type == AnimationGraphNodeType::Clip && !Node.Inputs.empty())
			throw std::invalid_argument("Animation clip graph nodes cannot have input nodes");
		if (Node.Type != AnimationGraphNodeType::Clip && Node.Inputs.empty())
			throw std::invalid_argument("Animation graph operation nodes require at least one input");
		if (Node.Type == AnimationGraphNodeType::Output && Node.Inputs.size() != 1U)
			throw std::invalid_argument("Animation graph output requires exactly one input");
		if (Node.ControllingParameter != 0)
		{
			const auto Parameter = ParameterTypes.find(Node.ControllingParameter);
			if (Parameter == ParameterTypes.end() || Parameter->second != AnimationParameterType::Scalar)
				throw std::invalid_argument("Animation graph control parameters must reference scalar definitions");
		}
		for (AnimationNodeID Input : Node.Inputs)
		{
			if (NodeMap.find(Input) == NodeMap.end())
				throw std::invalid_argument("Animation graph node references a missing input");
		}
	}
	std::unordered_set<AnimationNodeID> Visiting;
	std::unordered_set<AnimationNodeID> Visited;
	std::function<void(AnimationNodeID)> ValidateAcyclic = [&](AnimationNodeID ID)
	{
		if (Visited.contains(ID))
			return;
		if (!Visiting.insert(ID).second)
			throw std::invalid_argument("Animation graph contains a dependency cycle");
		for (AnimationNodeID Input : NodeMap.at(ID)->Inputs)
			ValidateAcyclic(Input);
		Visiting.erase(ID);
		Visited.insert(ID);
	};
	for (const auto &[id, node] : NodeMap)
	{
		(void)node;
		ValidateAcyclic(id);
	}
}

RetargetProfileAsset::RetargetProfileAsset(AssetHandle<SkeletonAsset> Source, AssetHandle<SkeletonAsset> Destination,
										   std::vector<RetargetJointMapping> Mappings)
	: Asset(util::UUID::GenerateRandomUUID()), Source(std::move(Source)), Destination(std::move(Destination)), Mappings(std::move(Mappings))
{
	if (!this->Source || !this->Destination || this->Mappings.empty())
	{
		throw std::invalid_argument("Retarget profile requires source/destination skeletons and mappings");
	}
	std::unordered_set<JointID> SourceJoints;
	std::unordered_set<JointID> DestinationJoints;
	auto SourceSkeleton = this->Source.Pin();
	auto DestinationSkeleton = this->Destination.Pin();
	for (const RetargetJointMapping &Mapping : this->Mappings)
	{
		if (Mapping.Source == 0 || Mapping.Destination == 0 || !SourceJoints.insert(Mapping.Source).second ||
			!DestinationJoints.insert(Mapping.Destination).second || SourceSkeleton->FindJoint(Mapping.Source) == nullptr ||
			DestinationSkeleton->FindJoint(Mapping.Destination) == nullptr || !std::isfinite(Mapping.TranslationScale) ||
			Mapping.TranslationScale <= 0.0f || !std::isfinite(Mapping.RotationOffset.w) || !std::isfinite(Mapping.RotationOffset.x) ||
			!std::isfinite(Mapping.RotationOffset.y) || !std::isfinite(Mapping.RotationOffset.z))
		{
			throw std::invalid_argument("Retarget mappings require unique joints and a positive translation scale");
		}
	}
}
} // namespace resource
