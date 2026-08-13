#include "AnimationAsset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace resource
{
namespace
{
constexpr usize MaximumAnimationTracks = 65'536;
constexpr usize MaximumAnimationKeys = 4'194'304;
constexpr usize MaximumAnimationEvents = 65'536;
constexpr usize MaximumAnimationGraphNodes = 65'536;
constexpr usize MaximumAnimationGraphParameters = 16'384;
constexpr usize MaximumAnimationGraphInputs = 256;
constexpr usize MaximumAnimationGraphEdges = 1'048'576;
constexpr usize MaximumAnimationGraphDepth = 256;
constexpr usize MaximumRetargetMappings = 65'536;

[[nodiscard]] bool IsFinite(const glm::quat &Value) noexcept
{
	return std::isfinite(Value.w) && std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

[[nodiscard]] float64 StableQuaternionNorm(const glm::quat &Value) noexcept
{
	const float64 Maximum = std::max({std::abs(static_cast<float64>(Value.w)), std::abs(static_cast<float64>(Value.x)),
									  std::abs(static_cast<float64>(Value.y)), std::abs(static_cast<float64>(Value.z))});
	if (!std::isfinite(Maximum) || Maximum == 0.0)
		return Maximum;
	const float64 W = static_cast<float64>(Value.w) / Maximum;
	const float64 X = static_cast<float64>(Value.x) / Maximum;
	const float64 Y = static_cast<float64>(Value.y) / Maximum;
	const float64 Z = static_cast<float64>(Value.z) / Maximum;
	return Maximum * std::sqrt(W * W + X * X + Y * Y + Z * Z);
}

[[nodiscard]] glm::quat NormalizeQuaternion(const glm::quat &Value)
{
	const float64 Norm = StableQuaternionNorm(Value);
	if (!std::isfinite(Norm) || Norm <= std::numeric_limits<float32>::epsilon())
		throw std::invalid_argument("Animation rotation quaternion must have a finite non-zero norm");
	return glm::quat(static_cast<float32>(static_cast<float64>(Value.w) / Norm), static_cast<float32>(static_cast<float64>(Value.x) / Norm),
					 static_cast<float32>(static_cast<float64>(Value.y) / Norm),
					 static_cast<float32>(static_cast<float64>(Value.z) / Norm));
}

[[nodiscard]] bool IsValidParameterType(const AnimationParameterType Type) noexcept
{
	return Type == AnimationParameterType::Boolean || Type == AnimationParameterType::Scalar || Type == AnimationParameterType::Vector;
}

[[nodiscard]] bool IsValidNodeType(const AnimationGraphNodeType Type) noexcept
{
	return Type == AnimationGraphNodeType::Clip || Type == AnimationGraphNodeType::Blend || Type == AnimationGraphNodeType::StateMachine ||
		   Type == AnimationGraphNodeType::Layer || Type == AnimationGraphNodeType::Additive || Type == AnimationGraphNodeType::Output;
}
} // namespace

AnimationClipAsset::AnimationClipAsset(string Name, AssetHandle<SkeletonAsset> Skeleton, float32 Duration, float32 SampleRate,
									   std::vector<AnimationJointTrack> JointTracks, std::vector<AnimationMorphTrack> MorphTracks,
									   std::vector<AnimationEvent> Events, RootMotionExtractionPolicy RootMotionPolicy,
									   JointID RootMotionJoint, AnimationInterpolationMode Interpolation, AnimationLoopPolicy LoopPolicy,
									   AnimationEventBoundaryPolicy EventBoundaryPolicy)
	: Asset(util::UUID::GenerateRandomUUID()), Name(std::move(Name)), Skeleton(std::move(Skeleton)), Duration(Duration),
	  SampleRate(SampleRate), JointTracks(std::move(JointTracks)), MorphTracks(std::move(MorphTracks)), Events(std::move(Events)),
	  RootMotionPolicy(RootMotionPolicy), RootMotionJoint(RootMotionJoint), Interpolation(Interpolation), LoopPolicy(LoopPolicy),
	  EventBoundaryPolicy(EventBoundaryPolicy)
{
	if (this->Name.empty() || !std::isfinite(this->Duration) || this->Duration <= 0.0f || !std::isfinite(this->SampleRate) ||
		this->SampleRate <= 0.0f)
	{
		throw std::invalid_argument("Animation clip requires a name, positive duration, and positive sample rate");
	}
	if (this->JointTracks.size() > MaximumAnimationTracks || this->MorphTracks.size() > MaximumAnimationTracks ||
		this->Events.size() > MaximumAnimationEvents)
		throw std::length_error("Animation clip exceeds engine track or event budgets");
	if ((!this->JointTracks.empty() || this->HasRootMotion()) && !this->Skeleton)
		throw std::invalid_argument("Joint animation and root motion require a skeleton");
	if ((this->RootMotionPolicy == RootMotionExtractionPolicy::Disabled) != (this->RootMotionJoint == 0) ||
		this->Interpolation != AnimationInterpolationMode::Linear || this->LoopPolicy != AnimationLoopPolicy::Loop ||
		this->EventBoundaryPolicy != AnimationEventBoundaryPolicy::IncludeStartThenPreviousExclusiveCurrentInclusive)
		throw std::invalid_argument("Animation clip root-motion or sampling policy is invalid");
	if (this->JointTracks.empty() && this->MorphTracks.empty())
		throw std::invalid_argument("Animation clip requires at least one joint or morph track");
	std::unordered_set<JointID> AnimatedJoints;
	std::unordered_set<AnimationTrackID> TrackIDs;
	AssetPtr<SkeletonAsset> PinnedSkeleton;
	if (this->Skeleton)
		PinnedSkeleton = this->Skeleton.Pin();
	usize TotalKeys = 0;
	for (AnimationJointTrack &Track : this->JointTracks)
	{
		if (Track.ID == 0 || Track.Joint == 0 || Track.Keys.empty() || !TrackIDs.insert(Track.ID).second ||
			!AnimatedJoints.insert(Track.Joint).second || PinnedSkeleton == nullptr || PinnedSkeleton->FindJoint(Track.Joint) == nullptr)
		{
			throw std::invalid_argument("Animation clip joint tracks are invalid or duplicated");
		}
		float32 PreviousTime = -1.0f;
		if (Track.Keys.size() > MaximumAnimationKeys - TotalKeys)
			throw std::length_error("Animation clip exceeds the engine key budget");
		TotalKeys += Track.Keys.size();
		for (AnimationTransformKey &Key : Track.Keys)
		{
			const bool FiniteTransform = std::isfinite(Key.Translation.x) && std::isfinite(Key.Translation.y) &&
										 std::isfinite(Key.Translation.z) && std::isfinite(Key.Rotation.w) &&
										 std::isfinite(Key.Rotation.x) && std::isfinite(Key.Rotation.y) && std::isfinite(Key.Rotation.z) &&
										 std::isfinite(Key.Scale.x) && std::isfinite(Key.Scale.y) && std::isfinite(Key.Scale.z);
			if (!std::isfinite(Key.Time) || Key.Time < PreviousTime || Key.Time < 0.0f || Key.Time > this->Duration || !FiniteTransform ||
				!IsFinite(Key.Rotation) || Key.Time <= PreviousTime || std::abs(Key.Scale.x) <= std::numeric_limits<float32>::epsilon() ||
				std::abs(Key.Scale.y) <= std::numeric_limits<float32>::epsilon() ||
				std::abs(Key.Scale.z) <= std::numeric_limits<float32>::epsilon())
			{
				throw std::invalid_argument("Animation clip keys must be finite, unique, and strictly ordered inside the clip duration");
			}
			Key.Rotation = NormalizeQuaternion(Key.Rotation);
			PreviousTime = Key.Time;
		}
	}
	if (this->HasRootMotion() && PinnedSkeleton->FindJoint(this->RootMotionJoint) == nullptr)
		throw std::invalid_argument("Animation root-motion joint is absent from the clip skeleton");
	if (this->HasRootMotion() && !AnimatedJoints.contains(this->RootMotionJoint))
		throw std::invalid_argument("Animation root-motion joint requires an animation track");
	std::unordered_set<uint64> AnimatedMorphTargets;
	for (const AnimationMorphTrack &Track : this->MorphTracks)
	{
		if (Track.MorphSet.empty() || Track.MorphTarget == 0 || Track.Keys.empty() ||
			!AnimatedMorphTargets.insert(Track.MorphTarget).second)
			throw std::invalid_argument("Animation morph tracks require unique stable targets and keys");
		if (Track.Keys.size() > MaximumAnimationKeys - TotalKeys)
			throw std::length_error("Animation clip exceeds the engine key budget");
		TotalKeys += Track.Keys.size();
		float32 PreviousTime = -1.0f;
		for (const AnimationMorphKey &Key : Track.Keys)
		{
			if (!std::isfinite(Key.Time) || !std::isfinite(Key.Weight) || Key.Weight < 0.0f || Key.Weight > 1.0f ||
				Key.Time < PreviousTime || Key.Time < 0.0f || Key.Time > this->Duration || Key.Time <= PreviousTime)
				throw std::invalid_argument("Animation morph keys must be finite, unique, and strictly ordered inside the clip duration");
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
	if (this->Nodes.size() > MaximumAnimationGraphNodes || this->Parameters.size() > MaximumAnimationGraphParameters)
		throw std::length_error("Animation graph exceeds engine node or parameter budgets");
	std::unordered_set<AnimationParameterID> ParameterIDs;
	std::unordered_set<string> ParameterNames;
	std::unordered_map<AnimationParameterID, AnimationParameterType> ParameterTypes;
	for (const auto &Parameter : this->Parameters)
	{
		if (!IsValidParameterType(Parameter.Type) || Parameter.ID == 0 || Parameter.Name.empty() ||
			!ParameterIDs.insert(Parameter.ID).second || !ParameterNames.insert(Parameter.Name).second ||
			!std::isfinite(Parameter.DefaultValue.x) || !std::isfinite(Parameter.DefaultValue.y) ||
			!std::isfinite(Parameter.DefaultValue.z) || !std::isfinite(Parameter.DefaultValue.w))
		{
			throw std::invalid_argument("Animation graph parameters require unique IDs and names");
		}
		if (Parameter.Type == AnimationParameterType::Boolean && Parameter.DefaultValue.x != 0.0f && Parameter.DefaultValue.x != 1.0f)
			throw std::invalid_argument("Animation Boolean parameter defaults must be zero or one");
		ParameterTypes.emplace(Parameter.ID, Parameter.Type);
	}
	std::unordered_map<AnimationNodeID, const AnimationGraphNode *> NodeMap;
	usize EdgeCount = 0;
	for (const AnimationGraphNode &Node : this->Nodes)
	{
		if (!IsValidNodeType(Node.Type) || Node.ID == 0 || !NodeMap.emplace(Node.ID, &Node).second ||
			Node.Inputs.size() > MaximumAnimationGraphInputs)
		{
			throw std::invalid_argument("Animation graph node IDs must be unique");
		}
		if (Node.Type == AnimationGraphNodeType::Clip && !Node.Clip)
		{
			throw std::invalid_argument("Animation graph clip node requires a clip handle");
		}
		std::unordered_set<AnimationNodeID> UniqueInputs;
		for (const AnimationNodeID Input : Node.Inputs)
			if (!UniqueInputs.insert(Input).second)
				throw std::invalid_argument("Animation graph node contains a duplicate input");
		if (Node.Inputs.size() > MaximumAnimationGraphEdges - EdgeCount)
			throw std::length_error("Animation graph exceeds the dependency-edge budget");
		EdgeCount += Node.Inputs.size();
	}
	if (NodeMap.find(this->OutputNode) == NodeMap.end() || NodeMap[this->OutputNode]->Type != AnimationGraphNodeType::Output)
	{
		throw std::invalid_argument("Animation graph output must reference an Output node");
	}
	for (const AnimationGraphNode &Node : this->Nodes)
	{
		if (Node.Type == AnimationGraphNodeType::Clip && (!Node.Inputs.empty() || Node.ControllingParameter != 0))
			throw std::invalid_argument("Animation clip graph nodes cannot have input nodes");
		if (Node.Type != AnimationGraphNodeType::Clip && Node.Inputs.empty())
			throw std::invalid_argument("Animation graph operation nodes require at least one input");
		if (Node.Type == AnimationGraphNodeType::Output && Node.Inputs.size() != 1U)
			throw std::invalid_argument("Animation graph output requires exactly one input");
		if ((Node.Type == AnimationGraphNodeType::Blend || Node.Type == AnimationGraphNodeType::Additive) && Node.Inputs.size() != 2U)
			throw std::invalid_argument("Animation blend and additive nodes require exactly two inputs");
		if (Node.Type == AnimationGraphNodeType::Layer && Node.Inputs.size() < 2U)
			throw std::invalid_argument("Animation layer nodes require a base pose and at least one layer");
		if (Node.Type == AnimationGraphNodeType::StateMachine && Node.ControllingParameter == 0)
			throw std::invalid_argument("Animation state-machine nodes require a scalar controlling parameter");
		if (Node.Type != AnimationGraphNodeType::Clip && Node.Clip)
			throw std::invalid_argument("Only animation clip nodes may retain clip assets");
		if ((Node.Type == AnimationGraphNodeType::Output || Node.Type == AnimationGraphNodeType::Layer) && Node.ControllingParameter != 0)
			throw std::invalid_argument("Animation output and layer nodes do not accept controlling parameters");
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
	AssetID GraphSkeleton;
	for (const AnimationGraphNode &Node : this->Nodes)
	{
		if (Node.Type != AnimationGraphNodeType::Clip)
			continue;
		AssetPtr<AnimationClipAsset> Clip = Node.Clip.Pin();
		if (!Clip->GetSkeleton())
			continue;
		const AssetID &Skeleton = Clip->GetSkeleton().GetID();
		if (GraphSkeleton.empty())
			GraphSkeleton = Skeleton;
		else if (GraphSkeleton != Skeleton)
			throw std::invalid_argument("Animation graph clip nodes use incompatible skeleton assets");
	}
	std::unordered_map<AnimationNodeID, usize> RemainingInputs;
	std::unordered_map<AnimationNodeID, usize> DependencyDepth;
	std::unordered_map<AnimationNodeID, std::vector<AnimationNodeID>> Dependents;
	std::vector<AnimationNodeID> Ready;
	for (const AnimationGraphNode &Node : this->Nodes)
	{
		RemainingInputs.emplace(Node.ID, Node.Inputs.size());
		if (Node.Inputs.empty())
		{
			DependencyDepth.emplace(Node.ID, 1);
			Ready.push_back(Node.ID);
		}
		for (const AnimationNodeID Input : Node.Inputs)
			Dependents[Input].push_back(Node.ID);
	}
	usize VisitedCount = 0;
	while (!Ready.empty())
	{
		const AnimationNodeID ID = Ready.back();
		Ready.pop_back();
		++VisitedCount;
		for (const AnimationNodeID Dependent : Dependents[ID])
		{
			usize &Depth = DependencyDepth[Dependent];
			Depth = std::max(Depth, DependencyDepth.at(ID) + 1U);
			if (Depth > MaximumAnimationGraphDepth)
				throw std::length_error("Animation graph exceeds the dependency-depth budget");
			usize &Count = RemainingInputs.at(Dependent);
			if (--Count == 0)
				Ready.push_back(Dependent);
		}
	}
	if (VisitedCount != this->Nodes.size())
		throw std::invalid_argument("Animation graph contains a dependency cycle");
}

RetargetProfileAsset::RetargetProfileAsset(AssetHandle<SkeletonAsset> Source, AssetHandle<SkeletonAsset> Destination,
										   std::vector<RetargetJointMapping> Mappings, RetargetTranslationUnitPolicy UnitPolicy)
	: Asset(util::UUID::GenerateRandomUUID()), Source(std::move(Source)), Destination(std::move(Destination)),
	  Mappings(std::move(Mappings)), UnitPolicy(UnitPolicy)
{
	if (!this->Source || !this->Destination || this->Mappings.empty())
	{
		throw std::invalid_argument("Retarget profile requires source/destination skeletons and mappings");
	}
	if (this->Mappings.size() > MaximumRetargetMappings || this->UnitPolicy != RetargetTranslationUnitPolicy::ExplicitPerJointScale)
		throw std::invalid_argument("Retarget profile exceeds engine limits or has an invalid unit policy");
	std::unordered_set<JointID> SourceJoints;
	std::unordered_set<JointID> DestinationJoints;
	auto SourceSkeleton = this->Source.Pin();
	auto DestinationSkeleton = this->Destination.Pin();
	this->SourceCompatibilitySignature = SourceSkeleton->GetCompatibilitySignature();
	this->DestinationCompatibilitySignature = DestinationSkeleton->GetCompatibilitySignature();
	std::unordered_map<JointID, JointID> DestinationBySource;
	for (RetargetJointMapping &Mapping : this->Mappings)
	{
		if (Mapping.Source == 0 || Mapping.Destination == 0 || !SourceJoints.insert(Mapping.Source).second ||
			!DestinationJoints.insert(Mapping.Destination).second || SourceSkeleton->FindJoint(Mapping.Source) == nullptr ||
			DestinationSkeleton->FindJoint(Mapping.Destination) == nullptr || !std::isfinite(Mapping.TranslationScale) ||
			Mapping.TranslationScale <= 0.0f || !IsFinite(Mapping.RotationOffset))
		{
			throw std::invalid_argument("Retarget mappings require unique joints and a positive translation scale");
		}
		Mapping.RotationOffset = NormalizeQuaternion(Mapping.RotationOffset);
		DestinationBySource.emplace(Mapping.Source, Mapping.Destination);
	}
	for (const RetargetJointMapping &Mapping : this->Mappings)
	{
		const SkeletonJoint *SourceJoint = SourceSkeleton->FindJoint(Mapping.Source);
		const SkeletonJoint *DestinationJoint = DestinationSkeleton->FindJoint(Mapping.Destination);
		const bool SourceIsRoot = SourceJoint->ParentIndex == InvalidJointIndex;
		const bool DestinationIsRoot = DestinationJoint->ParentIndex == InvalidJointIndex;
		if (SourceIsRoot != DestinationIsRoot)
			throw std::invalid_argument("Retarget profile maps root and non-root joints incompatibly");
		if (SourceIsRoot)
			continue;
		const JointID SourceParent = SourceSkeleton->GetJoints()[SourceJoint->ParentIndex].ID;
		const JointID DestinationParent = DestinationSkeleton->GetJoints()[DestinationJoint->ParentIndex].ID;
		const auto MappedParent = DestinationBySource.find(SourceParent);
		if (MappedParent == DestinationBySource.end() || MappedParent->second != DestinationParent)
			throw std::invalid_argument("Retarget profile does not preserve mapped joint topology");
	}
}
} // namespace resource
