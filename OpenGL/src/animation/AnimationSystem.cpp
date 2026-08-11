#include "AnimationSystem.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <gtc/matrix_transform.hpp>
#include <limits>
#include <ppl.h>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#define GLM_ENABLE_EXPERIMENTAL
#include <gtx/matrix_decompose.hpp>
#include <gtx/quaternion.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#include "src/component/object/CObjectAnimationComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/resource/asset/AnimationAsset.h"

namespace animation
{
namespace
{
struct JointTransform final
{
	glm::vec3 Translation{0.0f};
	glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 Scale{1.0f};
};

struct EvaluatedPose final
{
	struct MorphWeight final
	{
		resource::AssetID MorphSet;
		resource::MorphTargetID Target = 0;
		float32 Weight = 0.0f;
	};
	struct ContributingClip final
	{
		resource::AssetHandle<resource::AnimationClipAsset> Handle;
		resource::AssetPtr<resource::AnimationClipAsset> Asset;
	};

	resource::AssetHandle<resource::SkeletonAsset> Skeleton;
	resource::AssetPtr<resource::SkeletonAsset> PinnedSkeleton;
	std::vector<JointTransform> Joints;
	std::vector<MorphWeight> MorphWeights;
	std::vector<ContributingClip> ContributingClips;
};

template <typename T> [[nodiscard]] resource::AssetPtr<T> PinRequired(const resource::AssetHandle<T> &Handle, const string_view Role)
{
	resource::AssetPtr<T> Pinned = Handle.Pin();
	if (Pinned == nullptr)
		throw std::runtime_error(string(Role) + " is unavailable during animation evaluation");
	return Pinned;
}

[[nodiscard]] bool IsFinite(const glm::vec3 &Value) noexcept
{
	return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

[[nodiscard]] bool IsFinite(const glm::quat &Value) noexcept
{
	return std::isfinite(Value.w) && std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

[[nodiscard]] glm::quat NormalizeRotation(const glm::quat &Value)
{
	const float64 Maximum = std::max({std::abs(static_cast<float64>(Value.w)), std::abs(static_cast<float64>(Value.x)),
									  std::abs(static_cast<float64>(Value.y)), std::abs(static_cast<float64>(Value.z))});
	if (!std::isfinite(Maximum) || Maximum == 0.0)
		throw std::runtime_error("Animation evaluation produced a degenerate quaternion");
	const glm::dvec4 Scaled(static_cast<float64>(Value.w) / Maximum, static_cast<float64>(Value.x) / Maximum,
							static_cast<float64>(Value.y) / Maximum, static_cast<float64>(Value.z) / Maximum);
	const float64 Norm = Maximum * std::sqrt(glm::dot(Scaled, Scaled));
	if (!std::isfinite(Norm) || Norm <= std::numeric_limits<float32>::epsilon())
		throw std::runtime_error("Animation evaluation produced a non-finite quaternion norm");
	return {static_cast<float32>(static_cast<float64>(Value.w) / Norm), static_cast<float32>(static_cast<float64>(Value.x) / Norm),
			static_cast<float32>(static_cast<float64>(Value.y) / Norm), static_cast<float32>(static_cast<float64>(Value.z) / Norm)};
}

[[nodiscard]] JointTransform Decompose(const glm::mat4 &Matrix)
{
	JointTransform Result;
	glm::vec3 Skew;
	glm::vec4 Perspective;
	if (!glm::decompose(Matrix, Result.Scale, Result.Rotation, Result.Translation, Skew, Perspective))
	{
		throw std::runtime_error("Skeleton reference transform cannot be decomposed");
	}
	if (!IsFinite(Result.Scale) || !IsFinite(Result.Translation) || !IsFinite(Result.Rotation))
		throw std::runtime_error("Skeleton reference transform decomposed to non-finite values");
	Result.Rotation = NormalizeRotation(Result.Rotation);
	return Result;
}

[[nodiscard]] glm::mat4 Compose(const JointTransform &Transform)
{
	return glm::translate(glm::mat4(1.0f), Transform.Translation) * glm::toMat4(Transform.Rotation) *
		   glm::scale(glm::mat4(1.0f), Transform.Scale);
}

[[nodiscard]] JointTransform Interpolate(const JointTransform &Left, const JointTransform &Right, float32 Alpha)
{
	JointTransform Result{.Translation = glm::mix(Left.Translation, Right.Translation, Alpha),
						  .Rotation = NormalizeRotation(glm::slerp(Left.Rotation, Right.Rotation, Alpha)),
						  .Scale = glm::mix(Left.Scale, Right.Scale, Alpha)};
	if (!IsFinite(Result.Translation) || !IsFinite(Result.Scale))
		throw std::runtime_error("Animation interpolation produced non-finite transform data");
	return Result;
}

[[nodiscard]] JointTransform SampleTrack(const resource::AnimationJointTrack &Track, float32 Time)
{
	if (Time <= Track.Keys.front().Time)
	{
		const auto &Key = Track.Keys.front();
		return {Key.Translation, Key.Rotation, Key.Scale};
	}
	if (Time >= Track.Keys.back().Time)
	{
		const auto &Key = Track.Keys.back();
		return {Key.Translation, Key.Rotation, Key.Scale};
	}
	const auto Upper = std::upper_bound(Track.Keys.begin(), Track.Keys.end(), Time,
										[](float32 Value, const resource::AnimationTransformKey &Key) { return Value < Key.Time; });
	const auto Lower = Upper - 1;
	const float32 Interval = Upper->Time - Lower->Time;
	const float32 Alpha = Interval <= 0.0f ? 0.0f : (Time - Lower->Time) / Interval;
	return Interpolate({Lower->Translation, Lower->Rotation, Lower->Scale}, {Upper->Translation, Upper->Rotation, Upper->Scale}, Alpha);
}

[[nodiscard]] float32 SampleMorphTrack(const resource::AnimationMorphTrack &Track, float32 Time)
{
	if (Time <= Track.Keys.front().Time)
		return Track.Keys.front().Weight;
	if (Time >= Track.Keys.back().Time)
		return Track.Keys.back().Weight;
	const auto Upper = std::upper_bound(Track.Keys.begin(), Track.Keys.end(), Time,
										[](float32 Value, const resource::AnimationMorphKey &Key) { return Value < Key.Time; });
	const auto Lower = Upper - 1;
	const float32 Interval = Upper->Time - Lower->Time;
	return glm::mix(Lower->Weight, Upper->Weight, Interval <= 0.0f ? 0.0f : (Time - Lower->Time) / Interval);
}

[[nodiscard]] float32 ParameterScalar(const components::CObjectAnimationComponent &Component, const resource::AnimationGraphAsset &Graph,
									  resource::AnimationParameterID ID)
{
	const auto Parameters = Component.GetParameters();
	const auto Current = std::find_if(Parameters.begin(), Parameters.end(),
									  [ID](const components::AnimationParameterValue &Value) { return Value.ID == ID; });
	if (Current != Parameters.end())
		return Current->Value.x;
	const auto Definition = std::find_if(Graph.GetParameters().begin(), Graph.GetParameters().end(),
										 [ID](const resource::AnimationParameterDefinition &Value) { return Value.ID == ID; });
	return Definition == Graph.GetParameters().end() ? 0.0f : Definition->DefaultValue.x;
}

[[nodiscard]] EvaluatedPose SampleClip(const resource::AnimationClipAsset &Clip, float64 PlaybackTime)
{
	const float32 Time = static_cast<float32>(std::fmod(std::max(PlaybackTime, 0.0), static_cast<float64>(Clip.GetDuration())));
	EvaluatedPose Result;
	Result.Skeleton = Clip.GetSkeleton();
	if (Clip.GetSkeleton())
	{
		resource::AssetPtr<resource::SkeletonAsset> Skeleton = PinRequired(Clip.GetSkeleton(), "Animation clip skeleton");
		Result.PinnedSkeleton = Skeleton;
		Result.Joints.reserve(Skeleton->GetJoints().size());
		for (const resource::SkeletonJoint &Joint : Skeleton->GetJoints())
			Result.Joints.push_back(Decompose(Joint.ReferenceLocalTransform));
		for (const resource::AnimationJointTrack &Track : Clip.GetJointTracks())
		{
			const resource::SkeletonJoint *Joint = Skeleton->FindJoint(Track.Joint);
			if (Joint == nullptr)
				throw std::runtime_error("Animation clip track references a joint outside its skeleton");
			const uint32 JointIndex = static_cast<uint32>(Joint - Skeleton->GetJoints().data());
			Result.Joints[JointIndex] = SampleTrack(Track, Time);
		}
	}
	for (const resource::AnimationMorphTrack &Track : Clip.GetMorphTracks())
		Result.MorphWeights.push_back({Track.MorphSet, Track.MorphTarget, SampleMorphTrack(Track, Time)});
	return Result;
}

[[nodiscard]] EvaluatedPose Blend(EvaluatedPose Left, const EvaluatedPose &Right, float32 Alpha, bool Additive)
{
	if (Left.Skeleton.GetID() != Right.Skeleton.GetID() || Left.Joints.size() != Right.Joints.size())
	{
		throw std::runtime_error("Animation graph attempted to blend incompatible skeletons");
	}
	Alpha = glm::clamp(Alpha, 0.0f, 1.0f);
	for (uint32 Joint = 0; Joint < Left.Joints.size(); ++Joint)
	{
		if (!Additive)
			Left.Joints[Joint] = Interpolate(Left.Joints[Joint], Right.Joints[Joint], Alpha);
		else
		{
			Left.Joints[Joint].Translation += Right.Joints[Joint].Translation * Alpha;
			Left.Joints[Joint].Rotation = NormalizeRotation(
				Left.Joints[Joint].Rotation * glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), Right.Joints[Joint].Rotation, Alpha));
			Left.Joints[Joint].Scale *= glm::mix(glm::vec3(1.0f), Right.Joints[Joint].Scale, Alpha);
		}
	}
	for (const EvaluatedPose::MorphWeight &RightWeight : Right.MorphWeights)
	{
		auto Existing =
			std::find_if(Left.MorphWeights.begin(), Left.MorphWeights.end(), [&RightWeight](const EvaluatedPose::MorphWeight &Value)
						 { return Value.Target == RightWeight.Target && Value.MorphSet == RightWeight.MorphSet; });
		if (Existing == Left.MorphWeights.end())
			Left.MorphWeights.push_back(RightWeight);
		else
			Existing->Weight =
				Additive ? Existing->Weight + RightWeight.Weight * Alpha : glm::mix(Existing->Weight, RightWeight.Weight, Alpha);
	}
	for (const EvaluatedPose::ContributingClip &Clip : Right.ContributingClips)
	{
		const auto Found = std::find_if(Left.ContributingClips.begin(), Left.ContributingClips.end(),
										[&Clip](const EvaluatedPose::ContributingClip &Candidate)
										{ return Candidate.Handle.GetID() == Clip.Handle.GetID(); });
		if (Found == Left.ContributingClips.end())
			Left.ContributingClips.push_back(Clip);
	}
	return Left;
}

[[nodiscard]] EvaluatedPose EvaluateGraph(const resource::AnimationGraphAsset &Graph,
										  const components::CObjectAnimationComponent &Component, float64 PlaybackTime)
{
	std::unordered_map<resource::AnimationNodeID, const resource::AnimationGraphNode *> Nodes;
	for (const auto &Node : Graph.GetNodes())
		Nodes.emplace(Node.ID, &Node);
	std::unordered_set<resource::AnimationNodeID> Evaluating;
	std::function<EvaluatedPose(resource::AnimationNodeID)> Evaluate = [&](resource::AnimationNodeID ID) -> EvaluatedPose
	{
		const auto Found = Nodes.find(ID);
		if (Found == Nodes.end())
			throw std::runtime_error("Animation graph references a missing node");
		if (!Evaluating.insert(ID).second)
			throw std::runtime_error("Animation graph contains a cycle");
		const resource::AnimationGraphNode &Node = *Found->second;
		EvaluatedPose Result;
		if (Node.Type == resource::AnimationGraphNodeType::Clip)
		{
			resource::AssetPtr<resource::AnimationClipAsset> Clip = PinRequired(Node.Clip, "Animation graph clip");
			Result = SampleClip(*Clip, PlaybackTime);
			Result.ContributingClips.push_back({Node.Clip, std::move(Clip)});
		}
		else
		{
			if (Node.Inputs.empty())
				throw std::runtime_error("Animation graph operation node has no inputs");
			uint32 SelectedInput = 0;
			if (Node.Type == resource::AnimationGraphNodeType::StateMachine)
			{
				SelectedInput = static_cast<uint32>(glm::clamp(ParameterScalar(Component, Graph, Node.ControllingParameter), 0.0f,
															   static_cast<float32>(Node.Inputs.size() - 1U)));
				Result = Evaluate(Node.Inputs[SelectedInput]);
			}
			else
			{
				Result = Evaluate(Node.Inputs.front());
				const float32 Alpha = Node.ControllingParameter == 0 ? 1.0f : ParameterScalar(Component, Graph, Node.ControllingParameter);
				for (uint32 Input = 1; Input < Node.Inputs.size(); ++Input)
					Result = Blend(std::move(Result), Evaluate(Node.Inputs[Input]), Alpha,
								   Node.Type == resource::AnimationGraphNodeType::Additive);
			}
		}
		Evaluating.erase(ID);
		return Result;
	};
	return Evaluate(Graph.GetOutputNode());
}

void PublishAnimationEvents(components::CObjectAnimationComponent &Component, const EvaluatedPose &Pose,
							const components::AnimationPlaybackInterval &Playback)
{
	constexpr uint64 MaximumEventsPerUpdate = 65'536;
	if (Playback.Current <= Playback.Previous)
		return;
	uint64 Published = 0;
	for (const EvaluatedPose::ContributingClip &ContributingClip : Pose.ContributingClips)
	{
		const resource::AssetPtr<resource::AnimationClipAsset> &Clip = ContributingClip.Asset;
		if (Clip == nullptr)
			throw std::runtime_error("Animation event source clip lost its retained asset");
		if (Clip->GetEvents().empty())
			continue;
		const float64 Duration = static_cast<float64>(Clip->GetDuration());
		const uint64 FirstCycle = static_cast<uint64>(std::floor(Playback.Previous / Duration));
		const uint64 LastCycle = static_cast<uint64>(std::floor(Playback.Current / Duration));
		const uint64 CycleCount = LastCycle - FirstCycle + 1U;
		if (CycleCount > MaximumEventsPerUpdate / Clip->GetEvents().size())
			throw std::overflow_error("Animation update produced too many events");
		for (uint64 Cycle = FirstCycle; Cycle <= LastCycle; ++Cycle)
		{
			for (const resource::AnimationEvent &Event : Clip->GetEvents())
			{
				const float64 Occurrence = static_cast<float64>(Cycle) * Duration + static_cast<float64>(Event.Time);
				const bool InitialStartEvent = Playback.Previous == 0.0 && Cycle == 0 && Event.Time == 0.0f;
				if ((!InitialStartEvent && Occurrence <= Playback.Previous) || Occurrence > Playback.Current)
					continue;
				if (++Published > MaximumEventsPerUpdate)
					throw std::overflow_error("Animation update produced too many events");
				Component.PublishTriggeredEvent({ContributingClip.Handle.GetID(), Event.ID, Occurrence, Event.Name});
			}
			if (Cycle == std::numeric_limits<uint64>::max())
				break;
		}
	}
}

[[nodiscard]] std::vector<glm::mat4> BuildSkinPose(const resource::SkeletonAsset &Skeleton, std::span<const JointTransform> LocalPose)
{
	if (LocalPose.size() != Skeleton.GetJoints().size())
		throw std::runtime_error("Animation pose joint count does not match its skeleton");
	std::vector<glm::mat4> GlobalPose(LocalPose.size());
	std::vector<glm::mat4> SkinPose(LocalPose.size());
	for (uint32 JointIndex = 0; JointIndex < LocalPose.size(); ++JointIndex)
	{
		const resource::SkeletonJoint &Joint = Skeleton.GetJoints()[JointIndex];
		const glm::mat4 Local = Compose(LocalPose[JointIndex]);
		GlobalPose[JointIndex] = Joint.ParentIndex == resource::InvalidJointIndex ? Local : GlobalPose[Joint.ParentIndex] * Local;
		SkinPose[JointIndex] = GlobalPose[JointIndex] * Joint.InverseBindMatrix;
	}
	return SkinPose;
}

void UpdateComponent(world::Scene::WriteAccess &Access, components::CObjectAnimationComponent &Component, float32 DeltaSeconds)
{
	if (!Component.IsEnabled())
		return;
	const components::AnimationPlaybackInterval Playback = Component.AdvancePlayback(DeltaSeconds);
	resource::AssetPtr<resource::AnimationGraphAsset> Graph = PinRequired(Component.GetGraph(), "Animation component graph");
	EvaluatedPose Pose = EvaluateGraph(*Graph, Component, Playback.Current);
	PublishAnimationEvents(Component, Pose, Playback);
	resource::AssetPtr<resource::SkeletonAsset> Skeleton = Pose.PinnedSkeleton;
	if (Component.IsRootMotionEnabled())
	{
		if (Skeleton == nullptr)
			throw std::runtime_error("Root motion cannot be evaluated from a morph-only animation");
		EvaluatedPose PreviousPose = EvaluateGraph(*Graph, Component, Playback.Previous);
		if (PreviousPose.Skeleton.GetID() != Pose.Skeleton.GetID())
			throw std::runtime_error("Root-motion evaluation changed skeleton within one update");
		if (Pose.ContributingClips.empty())
			throw std::runtime_error("Root motion requires at least one contributing animation clip");
		const float64 LoopDuration = Pose.ContributingClips.front().Asset->GetDuration();
		for (const EvaluatedPose::ContributingClip &ContributingClip : Pose.ContributingClips)
		{
			if (ContributingClip.Asset == nullptr)
				throw std::runtime_error("Root-motion source clip lost its retained asset");
			if (std::abs(static_cast<float64>(ContributingClip.Asset->GetDuration()) - LoopDuration) >
				std::numeric_limits<float32>::epsilon())
			{
				throw std::runtime_error("Root-motion blends require synchronized clip durations");
			}
		}
		const resource::JointID RootMotionJoint = Pose.ContributingClips.front().Asset->GetRootMotionJoint();
		if (RootMotionJoint == 0)
			throw std::runtime_error("Root-motion clip does not declare a source joint");
		for (const EvaluatedPose::ContributingClip &ContributingClip : Pose.ContributingClips)
			if (ContributingClip.Asset->GetRootMotionJoint() != RootMotionJoint)
				throw std::runtime_error("Root-motion blends require the same declared source joint");
		const resource::SkeletonJoint *Root = Skeleton->FindJoint(RootMotionJoint);
		if (Root == nullptr)
			throw std::runtime_error("Root-motion source joint is absent from the evaluated skeleton");
		const uint32 RootIndex = static_cast<uint32>(Root - Skeleton->GetJoints().data());
		glm::vec3 TranslationDelta = Pose.Joints[RootIndex].Translation - PreviousPose.Joints[RootIndex].Translation;
		glm::quat RotationDelta =
			NormalizeRotation(glm::inverse(PreviousPose.Joints[RootIndex].Rotation) * Pose.Joints[RootIndex].Rotation);
		const uint64 PreviousCycle = static_cast<uint64>(std::floor(Playback.Previous / LoopDuration));
		const uint64 CurrentCycle = static_cast<uint64>(std::floor(Playback.Current / LoopDuration));
		if (CurrentCycle > PreviousCycle)
		{
			const uint64 CrossedCycles = CurrentCycle - PreviousCycle;
			if (CrossedCycles > 65'536)
				throw std::overflow_error("Root-motion update crossed too many animation loops");
			const float64 EndSampleTime = std::nextafter(LoopDuration, 0.0);
			const EvaluatedPose StartPose = EvaluateGraph(*Graph, Component, 0.0f);
			const EvaluatedPose EndPose = EvaluateGraph(*Graph, Component, EndSampleTime);
			const JointTransform &StartRoot = StartPose.Joints[RootIndex];
			const JointTransform &EndRoot = EndPose.Joints[RootIndex];
			TranslationDelta = (EndRoot.Translation - PreviousPose.Joints[RootIndex].Translation) +
							   (Pose.Joints[RootIndex].Translation - StartRoot.Translation) +
							   static_cast<float32>(CrossedCycles - 1U) * (EndRoot.Translation - StartRoot.Translation);
			RotationDelta = NormalizeRotation(glm::inverse(PreviousPose.Joints[RootIndex].Rotation) * EndRoot.Rotation);
			const glm::quat FullLoopRotation = NormalizeRotation(glm::inverse(StartRoot.Rotation) * EndRoot.Rotation);
			for (uint64 Cycle = 1; Cycle < CrossedCycles; ++Cycle)
				RotationDelta = NormalizeRotation(RotationDelta * FullLoopRotation);
			RotationDelta = NormalizeRotation(RotationDelta * glm::inverse(StartRoot.Rotation) * Pose.Joints[RootIndex].Rotation);
		}
		const auto TransformHandle = Access.GetComponent<components::CObjectTransformComponent>(Component.GetOwner());
		if (!TransformHandle.IsValid())
			throw std::runtime_error("Animation root motion requires the owner's Transform component");
		auto &Transform = Access.Resolve(TransformHandle);
		Transform.Translate(TranslationDelta);
		Transform.SetRotation(NormalizeRotation(Transform.GetRotation() * RotationDelta));
		Pose.Joints[RootIndex] = Decompose(Root->ReferenceLocalTransform);
	}
	if (Skeleton != nullptr)
	{
		std::vector<glm::mat4> SkinPose = BuildSkinPose(*Skeleton, Pose.Joints);
		Component.PublishRigPose(Pose.Skeleton.GetID(), std::move(SkinPose));
		for (const auto &ProfileHandle : Component.GetRetargetProfiles())
		{
			resource::AssetPtr<resource::RetargetProfileAsset> Profile = PinRequired(ProfileHandle, "Animation retarget profile");
			if (Profile->GetSource().GetID() != Pose.Skeleton.GetID())
				continue;
			resource::AssetPtr<resource::SkeletonAsset> DestinationSkeleton =
				PinRequired(Profile->GetDestination(), "Animation retarget destination skeleton");
			std::vector<JointTransform> DestinationPose;
			DestinationPose.reserve(DestinationSkeleton->GetJoints().size());
			for (const resource::SkeletonJoint &Joint : DestinationSkeleton->GetJoints())
				DestinationPose.push_back(Decompose(Joint.ReferenceLocalTransform));
			for (const resource::RetargetJointMapping &Mapping : Profile->GetMappings())
			{
				const resource::SkeletonJoint *SourceJoint = Skeleton->FindJoint(Mapping.Source);
				const resource::SkeletonJoint *DestinationJoint = DestinationSkeleton->FindJoint(Mapping.Destination);
				if (SourceJoint == nullptr || DestinationJoint == nullptr)
					throw std::runtime_error("Validated retarget profile lost a skeleton joint");
				const uint32 SourceIndex = static_cast<uint32>(SourceJoint - Skeleton->GetJoints().data());
				const uint32 DestinationIndex = static_cast<uint32>(DestinationJoint - DestinationSkeleton->GetJoints().data());
				const JointTransform SourceReference = Decompose(SourceJoint->ReferenceLocalTransform);
				const JointTransform DestinationReference = Decompose(DestinationJoint->ReferenceLocalTransform);
				const JointTransform &SourceAnimated = Pose.Joints[SourceIndex];
				DestinationPose[DestinationIndex].Translation =
					DestinationReference.Translation +
					(SourceAnimated.Translation - SourceReference.Translation) * Mapping.TranslationScale;
				const glm::quat SourceRotationDelta = NormalizeRotation(SourceAnimated.Rotation * glm::inverse(SourceReference.Rotation));
				DestinationPose[DestinationIndex].Rotation =
					NormalizeRotation(DestinationReference.Rotation * Mapping.RotationOffset * SourceRotationDelta);
				DestinationPose[DestinationIndex].Scale =
					DestinationReference.Scale * (SourceAnimated.Scale / glm::max(SourceReference.Scale, glm::vec3(0.0001f)));
			}
			Component.PublishRigPose(Profile->GetDestination().GetID(), BuildSkinPose(*DestinationSkeleton, DestinationPose));
		}
	}
	Component.BeginMorphEvaluation();
	for (const EvaluatedPose::MorphWeight &Weight : Pose.MorphWeights)
	{
		Component.ApplyEvaluatedMorphWeight(Weight.MorphSet, Weight.Target, Weight.Weight);
	}
}
} // namespace

void AnimationSystem::Update(world::Scene &Scene, float32 DeltaSeconds) const
{
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f)
		throw std::invalid_argument("Animation update requires a finite non-negative delta time");
	auto Access = Scene.Write();
	auto Components = Access.Components<components::CObjectAnimationComponent>();
	if (Components.empty())
		return;
	concurrency::parallel_for(uint32{0}, static_cast<uint32>(Components.size()), [&Access, Components, DeltaSeconds](uint32 Index)
							  { UpdateComponent(Access, Components[Index], DeltaSeconds); });
}
} // namespace animation
