#pragma once

#include "src/component/object/CObjectComponent.h"
#include "src/resource/asset/AnimationAsset.h"
#include "src/resource/asset/MeshAsset.h"

#include <glm.hpp>
#include <span>
#include <vector>

namespace animation
{
class AnimationSystem;
}

namespace components
{
enum class AnimationUpdateMode : uint8
{
	Always,
	VisibleOnly,
	FixedRate
};

struct AnimationParameterValue final
{
	resource::AnimationParameterID ID = 0;
	resource::AnimationParameterType Type = resource::AnimationParameterType::Scalar;
	glm::vec4 Value{0.0f};
};

struct AnimationRigRuntimeState final
{
	resource::AssetID Skeleton;
	std::vector<glm::mat4> PreviousPose;
	std::vector<glm::mat4> CurrentPose;
	resource::AssetHandle<resource::RetargetProfileAsset> RetargetProfile;
};

struct AnimationMorphWeight final
{
	resource::MorphTargetID Target = 0;
	float32 Weight = 0.0f;
	float32 PreviousWeight = 0.0f;
};

struct AnimationPlaybackInterval final
{
	float64 Previous = 0.0;
	float64 Current = 0.0;
};

struct TriggeredAnimationEvent final
{
	resource::AssetID Clip;
	uint64 ID = 0;
	float64 AbsolutePlaybackTime = 0.0;
	string Name;
};

class CObjectAnimationComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<CObjectMeshComponent>;
	CCOMPONENT_BODY(CObjectAnimationComponent)

	explicit CObjectAnimationComponent(world::ObjectHandle Owner, resource::AssetHandle<resource::AnimationGraphAsset> Graph);

	[[nodiscard]] const resource::AssetHandle<resource::AnimationGraphAsset> &GetGraph() const noexcept
	{
		return this->Graph;
	}
	void SetGraph(resource::AssetHandle<resource::AnimationGraphAsset> Graph);
	void SetParameter(resource::AnimationParameterID Parameter, resource::AnimationParameterType Type, const glm::vec4 &Value);
	[[nodiscard]] std::span<const AnimationParameterValue> GetParameters() const noexcept
	{
		return this->Parameters;
	}
	void SetMorphWeight(resource::MorphTargetID Target, float32 Weight);
	[[nodiscard]] std::span<const AnimationMorphWeight> GetMorphWeights() const noexcept
	{
		return this->MorphWeights;
	}
	void SetRetargetProfile(resource::AssetHandle<resource::RetargetProfileAsset> Profile);
	void ClearRetargetProfiles() noexcept
	{
		this->RetargetProfiles.clear();
	}
	[[nodiscard]] std::span<const resource::AssetHandle<resource::RetargetProfileAsset>> GetRetargetProfiles() const noexcept
	{
		return this->RetargetProfiles;
	}
	[[nodiscard]] std::span<AnimationRigRuntimeState> GetRigStates() noexcept
	{
		return this->RigStates;
	}
	[[nodiscard]] std::span<const AnimationRigRuntimeState> GetRigStates() const noexcept
	{
		return this->RigStates;
	}
	[[nodiscard]] AnimationUpdateMode GetUpdateMode() const noexcept
	{
		return this->UpdateMode;
	}
	void SetUpdateMode(AnimationUpdateMode Value) noexcept
	{
		this->UpdateMode = Value;
	}
	[[nodiscard]] bool IsRootMotionEnabled() const noexcept
	{
		return this->RootMotionEnabled;
	}
	void SetRootMotionEnabled(bool Value) noexcept
	{
		this->RootMotionEnabled = Value;
	}
	[[nodiscard]] AnimationPlaybackInterval AdvancePlayback(float32 DeltaSeconds) noexcept;
	void PublishRigPose(resource::AssetID Skeleton, std::vector<glm::mat4> Pose);
	void BeginMorphEvaluation() noexcept;
	void ApplyEvaluatedMorphWeight(resource::MorphTargetID Target, float32 Weight);
	void PublishTriggeredEvent(TriggeredAnimationEvent Event);
	[[nodiscard]] std::span<const TriggeredAnimationEvent> GetTriggeredEvents() const noexcept
	{
		return this->TriggeredEvents;
	}
	[[nodiscard]] std::vector<TriggeredAnimationEvent> ConsumeTriggeredEvents();

	void OnAttachment() override;

  private:
	resource::AssetHandle<resource::AnimationGraphAsset> Graph;
	std::vector<AnimationParameterValue> Parameters;
	std::vector<AnimationMorphWeight> MorphWeights;
	std::vector<AnimationRigRuntimeState> RigStates;
	std::vector<resource::AssetHandle<resource::RetargetProfileAsset>> RetargetProfiles;
	std::vector<TriggeredAnimationEvent> TriggeredEvents;
	AnimationUpdateMode UpdateMode = AnimationUpdateMode::VisibleOnly;
	bool RootMotionEnabled = false;
	float64 PlaybackTime = 0.0;
	float64 PreviousPlaybackTime = 0.0;
};
} // namespace components
