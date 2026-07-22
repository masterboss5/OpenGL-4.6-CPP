#include "CObjectAnimationComponent.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace components
{
CObjectAnimationComponent::CObjectAnimationComponent(world::ObjectHandle Owner, resource::AssetHandle<resource::AnimationGraphAsset> Graph)
	: CObjectComponent(Owner), Graph(std::move(Graph))
{
	if (!this->Graph)
		throw std::invalid_argument("Animation component requires an AnimationGraphAsset handle");
}

void CObjectAnimationComponent::SetGraph(resource::AssetHandle<resource::AnimationGraphAsset> Replacement)
{
	if (!Replacement)
		throw std::invalid_argument("Animation component requires an AnimationGraphAsset handle");
	(void)Replacement.Pin();
	this->Graph = std::move(Replacement);
	this->Parameters.clear();
	this->RigStates.clear();
	this->TriggeredEvents.clear();
	this->PlaybackTime = 0.0f;
	this->PreviousPlaybackTime = 0.0f;
}

void CObjectAnimationComponent::SetParameter(resource::AnimationParameterID Parameter, resource::AnimationParameterType Type,
											 const glm::vec4 &Value)
{
	if (!std::isfinite(Value.x) || !std::isfinite(Value.y) || !std::isfinite(Value.z) || !std::isfinite(Value.w))
		throw std::invalid_argument("Animation parameter values must be finite");
	auto PinnedGraph = this->Graph.Pin();
	const auto Definition =
		std::find_if(PinnedGraph->GetParameters().begin(), PinnedGraph->GetParameters().end(),
					 [Parameter](const resource::AnimationParameterDefinition &Candidate) { return Candidate.ID == Parameter; });
	if (Definition == PinnedGraph->GetParameters().end() || Definition->Type != Type)
	{
		throw std::invalid_argument("Animation parameter is missing or has the wrong type");
	}
	auto Current = std::find_if(this->Parameters.begin(), this->Parameters.end(),
								[Parameter](const AnimationParameterValue &Candidate) { return Candidate.ID == Parameter; });
	if (Current == this->Parameters.end())
		this->Parameters.push_back({Parameter, Type, Value});
	else
		Current->Value = Value;
}

void CObjectAnimationComponent::SetMorphWeight(resource::MorphTargetID Target, float32 Weight)
{
	if (Target == 0 || !std::isfinite(Weight) || Weight < 0.0f || Weight > 1.0f)
	{
		throw std::invalid_argument("Animation morph weights require a stable target ID and normalized weight");
	}
	auto Current = std::find_if(this->MorphWeights.begin(), this->MorphWeights.end(),
								[Target](const AnimationMorphWeight &Candidate) { return Candidate.Target == Target; });
	if (Current == this->MorphWeights.end())
		this->MorphWeights.push_back({Target, Weight, Weight});
	else
	{
		Current->PreviousWeight = Current->Weight;
		Current->Weight = Weight;
	}
}

void CObjectAnimationComponent::SetRetargetProfile(resource::AssetHandle<resource::RetargetProfileAsset> Profile)
{
	if (!Profile)
		throw std::invalid_argument("Animation retarget profile handle cannot be empty");
	auto Pinned = Profile.Pin();
	const resource::AssetID Destination = Pinned->GetDestination().GetID();
	const auto Existing = std::find_if(this->RetargetProfiles.begin(), this->RetargetProfiles.end(), [&Destination](const auto &Candidate)
									   { return Candidate.Pin()->GetDestination().GetID() == Destination; });
	if (Existing == this->RetargetProfiles.end())
		this->RetargetProfiles.push_back(std::move(Profile));
	else
		*Existing = std::move(Profile);
}

void CObjectAnimationComponent::OnAttachment()
{
	(void)this->Graph.Pin();
}

AnimationPlaybackInterval CObjectAnimationComponent::AdvancePlayback(float32 DeltaSeconds) noexcept
{
	this->PreviousPlaybackTime = this->PlaybackTime;
	this->PlaybackTime += DeltaSeconds;
	return {this->PreviousPlaybackTime, this->PlaybackTime};
}

void CObjectAnimationComponent::PublishRigPose(resource::AssetID Skeleton, std::vector<glm::mat4> Pose)
{
	auto State = std::find_if(this->RigStates.begin(), this->RigStates.end(),
							  [&Skeleton](const AnimationRigRuntimeState &Value) { return Value.Skeleton == Skeleton; });
	if (State == this->RigStates.end())
	{
		this->RigStates.push_back({std::move(Skeleton), Pose, std::move(Pose), {}});
	}
	else
	{
		State->PreviousPose = State->CurrentPose.empty() ? Pose : std::move(State->CurrentPose);
		State->CurrentPose = std::move(Pose);
	}
}

void CObjectAnimationComponent::ApplyEvaluatedMorphWeight(resource::MorphTargetID Target, float32 Weight)
{
	if (Target == 0 || !std::isfinite(Weight))
		throw std::invalid_argument("Evaluated morph weights require a stable target ID and finite weight");
	Weight = glm::clamp(Weight, 0.0f, 1.0f);
	auto Current = std::find_if(this->MorphWeights.begin(), this->MorphWeights.end(),
								[Target](const AnimationMorphWeight &Value) { return Value.Target == Target; });
	if (Current == this->MorphWeights.end())
		this->MorphWeights.push_back({Target, Weight, Weight});
	else
		Current->Weight = Weight;
}

void CObjectAnimationComponent::BeginMorphEvaluation() noexcept
{
	for (AnimationMorphWeight &Weight : this->MorphWeights)
	{
		Weight.PreviousWeight = Weight.Weight;
		Weight.Weight = 0.0f;
	}
}

void CObjectAnimationComponent::PublishTriggeredEvent(TriggeredAnimationEvent Event)
{
	if (Event.Clip.empty() || Event.ID == 0 || Event.Name.empty() || !std::isfinite(Event.AbsolutePlaybackTime))
	{
		throw std::invalid_argument("Triggered animation event is incomplete");
	}
	this->TriggeredEvents.push_back(std::move(Event));
}

std::vector<TriggeredAnimationEvent> CObjectAnimationComponent::ConsumeTriggeredEvents()
{
	std::vector<TriggeredAnimationEvent> Result;
	Result.swap(this->TriggeredEvents);
	return Result;
}
} // namespace components
