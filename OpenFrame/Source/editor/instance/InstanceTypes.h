#pragma once

#include "Source/resource/asset/AssetTypes.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <compare>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm.hpp>
#include <gtc/quaternion.hpp>

namespace editor::instance
{
using InstanceClassID = util::UUID;

struct InstanceHandle final
{
	uint32 Slot = ~uint32{0};
	uint32 Generation = 0;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Slot != ~uint32{0} && this->Generation != 0;
	}

	[[nodiscard]] auto operator<=>(const InstanceHandle &) const noexcept = default;
};

struct InstanceAssetReference final
{
	resource::AssetID ID;
	resource::AssetType Type = resource::AssetType::Count;
	string ProjectRelativePath;

	[[nodiscard]] bool operator==(const InstanceAssetReference &) const noexcept = default;
};

using InstancePropertyValue = std::variant<bool, int32, uint32, int64, uint64, float32, float64, string, glm::vec2, glm::vec3, glm::vec4,
										   glm::quat, util::UUID, InstanceAssetReference>;
using InstancePropertyMap = std::unordered_map<string, InstancePropertyValue>;

enum class InstancePropertyPresentation : uint8
{
	Default,
	Color,
	Rotation,
	Choice,
	Asset
};

struct InstancePropertyDescriptor final
{
	string Name;
	string DisplayName;
	string Category = "Data";
	InstancePropertyValue DefaultValue;
	InstancePropertyPresentation Presentation = InstancePropertyPresentation::Default;
	std::optional<float64> Minimum;
	std::optional<float64> Maximum;
	float64 Step = 0.05;
	std::vector<string> Choices;
	bool ReadOnly = false;
};

enum class InstanceAvailability : uint8
{
	Available,
	Unavailable
};

enum class InstanceActivationState : uint8
{
	Active,
	Inactive,
	Unavailable
};

struct InstanceActivation final
{
	InstanceActivationState State = InstanceActivationState::Inactive;
	string Diagnostic;
};

struct InstanceRecord final
{
	util::UUID ID;
	InstanceClassID ClassID;
	string ClassName;
	string Name;
	util::UUID Parent;
	std::vector<util::UUID> Children;
	InstancePropertyMap Properties;
	uint32 SiblingOrder = 0;
	bool Enabled = true;
	bool Protected = false;
};

namespace class_ids
{
inline constexpr InstanceClassID Workspace{0x8fdb7fe334524239ULL, 0x8b8cb6df4d370001ULL};
inline constexpr InstanceClassID Lighting{0x8fdb7fe334524239ULL, 0x8b8cb6df4d370002ULL};
inline constexpr InstanceClassID GUI{0x8fdb7fe334524239ULL, 0x8b8cb6df4d370003ULL};
inline constexpr InstanceClassID Audio{0x8fdb7fe334524239ULL, 0x8b8cb6df4d370004ULL};
inline constexpr InstanceClassID Scripts{0x8fdb7fe334524239ULL, 0x8b8cb6df4d370005ULL};

inline constexpr InstanceClassID Folder{0x8fdb7fe334524239ULL, 0x8b8cb6df4d371001ULL};
inline constexpr InstanceClassID Model{0x8fdb7fe334524239ULL, 0x8b8cb6df4d371002ULL};
inline constexpr InstanceClassID Part{0x8fdb7fe334524239ULL, 0x8b8cb6df4d371003ULL};
inline constexpr InstanceClassID MeshPart{0x8fdb7fe334524239ULL, 0x8b8cb6df4d371004ULL};
inline constexpr InstanceClassID Camera{0x8fdb7fe334524239ULL, 0x8b8cb6df4d371005ULL};
inline constexpr InstanceClassID Attachment{0x8fdb7fe334524239ULL, 0x8b8cb6df4d371006ULL};

inline constexpr InstanceClassID DirectionalLight{0x8fdb7fe334524239ULL, 0x8b8cb6df4d372001ULL};
inline constexpr InstanceClassID PointLight{0x8fdb7fe334524239ULL, 0x8b8cb6df4d372002ULL};
inline constexpr InstanceClassID SpotLight{0x8fdb7fe334524239ULL, 0x8b8cb6df4d372003ULL};
inline constexpr InstanceClassID Sky{0x8fdb7fe334524239ULL, 0x8b8cb6df4d372004ULL};
inline constexpr InstanceClassID Atmosphere{0x8fdb7fe334524239ULL, 0x8b8cb6df4d372005ULL};
inline constexpr InstanceClassID Decal{0x8fdb7fe334524239ULL, 0x8b8cb6df4d372006ULL};
inline constexpr InstanceClassID ParticleEmitter{0x8fdb7fe334524239ULL, 0x8b8cb6df4d372007ULL};

inline constexpr InstanceClassID Animator{0x8fdb7fe334524239ULL, 0x8b8cb6df4d373001ULL};
inline constexpr InstanceClassID AnimationTrack{0x8fdb7fe334524239ULL, 0x8b8cb6df4d373002ULL};
inline constexpr InstanceClassID Script{0x8fdb7fe334524239ULL, 0x8b8cb6df4d374001ULL};
inline constexpr InstanceClassID ModuleScript{0x8fdb7fe334524239ULL, 0x8b8cb6df4d374002ULL};

inline constexpr InstanceClassID Sound{0x8fdb7fe334524239ULL, 0x8b8cb6df4d375001ULL};
inline constexpr InstanceClassID AudioEmitter{0x8fdb7fe334524239ULL, 0x8b8cb6df4d375002ULL};
inline constexpr InstanceClassID AudioListener{0x8fdb7fe334524239ULL, 0x8b8cb6df4d375003ULL};
inline constexpr InstanceClassID AudioGroup{0x8fdb7fe334524239ULL, 0x8b8cb6df4d375004ULL};

inline constexpr InstanceClassID ScreenGUI{0x8fdb7fe334524239ULL, 0x8b8cb6df4d376001ULL};
inline constexpr InstanceClassID Frame{0x8fdb7fe334524239ULL, 0x8b8cb6df4d376002ULL};
inline constexpr InstanceClassID Text{0x8fdb7fe334524239ULL, 0x8b8cb6df4d376003ULL};
inline constexpr InstanceClassID Image{0x8fdb7fe334524239ULL, 0x8b8cb6df4d376004ULL};
inline constexpr InstanceClassID Button{0x8fdb7fe334524239ULL, 0x8b8cb6df4d376005ULL};
inline constexpr InstanceClassID ScrollView{0x8fdb7fe334524239ULL, 0x8b8cb6df4d376006ULL};

inline constexpr InstanceClassID RigidBody{0x8fdb7fe334524239ULL, 0x8b8cb6df4d377001ULL};
inline constexpr InstanceClassID BoxCollider{0x8fdb7fe334524239ULL, 0x8b8cb6df4d377002ULL};
inline constexpr InstanceClassID SphereCollider{0x8fdb7fe334524239ULL, 0x8b8cb6df4d377003ULL};
inline constexpr InstanceClassID CapsuleCollider{0x8fdb7fe334524239ULL, 0x8b8cb6df4d377004ULL};
inline constexpr InstanceClassID MeshCollider{0x8fdb7fe334524239ULL, 0x8b8cb6df4d377005ULL};
inline constexpr InstanceClassID HingeConstraint{0x8fdb7fe334524239ULL, 0x8b8cb6df4d377006ULL};
inline constexpr InstanceClassID SpringConstraint{0x8fdb7fe334524239ULL, 0x8b8cb6df4d377007ULL};
} // namespace class_ids
} // namespace editor::instance
