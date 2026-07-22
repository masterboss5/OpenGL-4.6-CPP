#pragma once

#include "src/resource/Asset.h"
#include "src/resource/asset/AssetHandle.h"
#include "src/resource/asset/SkeletonAsset.h"

#include <glm.hpp>
#include <gtc/quaternion.hpp>
#include <span>
#include <vector>

namespace resource
{
using AnimationTrackID = uint64;
using AnimationNodeID = uint64;
using AnimationParameterID = uint64;

struct AnimationTransformKey final
{
	float32 Time = 0.0f;
	glm::vec3 Translation{0.0f};
	glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 Scale{1.0f};
};

struct AnimationJointTrack final
{
	AnimationTrackID ID = 0;
	JointID Joint = 0;
	std::vector<AnimationTransformKey> Keys;
};

struct AnimationMorphKey final
{
	float32 Time = 0.0f;
	float32 Weight = 0.0f;
};

struct AnimationMorphTrack final
{
	uint64 MorphTarget = 0;
	std::vector<AnimationMorphKey> Keys;
};

struct AnimationEvent final
{
	uint64 ID = 0;
	float32 Time = 0.0f;
	string Name;
};

class AnimationClipAsset final : public Asset
{
  public:
	inline static constexpr AssetType AssetType = AssetType::AnimationClip;
	AnimationClipAsset(string Name, AssetHandle<SkeletonAsset> Skeleton, float32 Duration, float32 SampleRate,
					   std::vector<AnimationJointTrack> JointTracks, std::vector<AnimationMorphTrack> MorphTracks,
					   std::vector<AnimationEvent> Events, bool ContainsRootMotion);

	[[nodiscard]] string_view GetName() const noexcept
	{
		return this->Name;
	}
	[[nodiscard]] const AssetHandle<SkeletonAsset> &GetSkeleton() const noexcept
	{
		return this->Skeleton;
	}
	[[nodiscard]] float32 GetDuration() const noexcept
	{
		return this->Duration;
	}
	[[nodiscard]] float32 GetSampleRate() const noexcept
	{
		return this->SampleRate;
	}
	[[nodiscard]] std::span<const AnimationJointTrack> GetJointTracks() const noexcept
	{
		return this->JointTracks;
	}
	[[nodiscard]] std::span<const AnimationMorphTrack> GetMorphTracks() const noexcept
	{
		return this->MorphTracks;
	}
	[[nodiscard]] std::span<const AnimationEvent> GetEvents() const noexcept
	{
		return this->Events;
	}
	[[nodiscard]] bool HasRootMotion() const noexcept
	{
		return this->ContainsRootMotion;
	}

  private:
	string Name;
	AssetHandle<SkeletonAsset> Skeleton;
	float32 Duration = 0.0f;
	float32 SampleRate = 0.0f;
	std::vector<AnimationJointTrack> JointTracks;
	std::vector<AnimationMorphTrack> MorphTracks;
	std::vector<AnimationEvent> Events;
	bool ContainsRootMotion = false;
};

enum class AnimationParameterType : uint8
{
	Boolean,
	Scalar,
	Vector
};

struct AnimationParameterDefinition final
{
	AnimationParameterID ID = 0;
	string Name;
	AnimationParameterType Type = AnimationParameterType::Scalar;
	glm::vec4 DefaultValue{0.0f};
};

enum class AnimationGraphNodeType : uint8
{
	Clip,
	Blend,
	StateMachine,
	Layer,
	Additive,
	Output
};

struct AnimationGraphNode final
{
	AnimationNodeID ID = 0;
	AnimationGraphNodeType Type = AnimationGraphNodeType::Clip;
	std::vector<AnimationNodeID> Inputs;
	AssetHandle<AnimationClipAsset> Clip;
	AnimationParameterID ControllingParameter = 0;
};

class AnimationGraphAsset final : public Asset
{
  public:
	inline static constexpr AssetType AssetType = AssetType::AnimationGraph;
	AnimationGraphAsset(string Name, std::vector<AnimationParameterDefinition> Parameters, std::vector<AnimationGraphNode> Nodes,
						AnimationNodeID OutputNode);

	[[nodiscard]] string_view GetName() const noexcept
	{
		return this->Name;
	}
	[[nodiscard]] std::span<const AnimationParameterDefinition> GetParameters() const noexcept
	{
		return this->Parameters;
	}
	[[nodiscard]] std::span<const AnimationGraphNode> GetNodes() const noexcept
	{
		return this->Nodes;
	}
	[[nodiscard]] AnimationNodeID GetOutputNode() const noexcept
	{
		return this->OutputNode;
	}

  private:
	string Name;
	std::vector<AnimationParameterDefinition> Parameters;
	std::vector<AnimationGraphNode> Nodes;
	AnimationNodeID OutputNode = 0;
};

struct RetargetJointMapping final
{
	JointID Source = 0;
	JointID Destination = 0;
	glm::quat RotationOffset{1.0f, 0.0f, 0.0f, 0.0f};
	float32 TranslationScale = 1.0f;
};

class RetargetProfileAsset final : public Asset
{
  public:
	inline static constexpr AssetType AssetType = AssetType::RetargetProfile;
	RetargetProfileAsset(AssetHandle<SkeletonAsset> Source, AssetHandle<SkeletonAsset> Destination,
						 std::vector<RetargetJointMapping> Mappings);

	[[nodiscard]] const AssetHandle<SkeletonAsset> &GetSource() const noexcept
	{
		return this->Source;
	}
	[[nodiscard]] const AssetHandle<SkeletonAsset> &GetDestination() const noexcept
	{
		return this->Destination;
	}
	[[nodiscard]] std::span<const RetargetJointMapping> GetMappings() const noexcept
	{
		return this->Mappings;
	}

  private:
	AssetHandle<SkeletonAsset> Source;
	AssetHandle<SkeletonAsset> Destination;
	std::vector<RetargetJointMapping> Mappings;
};
} // namespace resource
