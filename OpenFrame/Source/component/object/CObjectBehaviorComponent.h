#pragma once

#include "Source/component/object/CObjectComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <unordered_map>
#include <optional>
#include <variant>
#include <vector>

#include <glm.hpp>
#include <gtc/quaternion.hpp>

namespace components
{
using BehaviorTypeID = uint64;
using BehaviorPropertyValue =
	std::variant<bool, int32, uint32, int64, uint64, float32, float64, string, glm::vec2, glm::vec3, glm::vec4, glm::quat, util::UUID>;

[[nodiscard]] ENGINE_API util::UUID MakeBehaviorStableTypeID(string_view ModuleName, string_view TypeName) noexcept;

enum class BehaviorExecutionState : uint8
{
	Unresolved,
	Constructed,
	Active,
	Suspended,
	Failed
};

struct BehaviorInstance final
{
	util::UUID InstanceID = util::UUID::GenerateRandomUUID();
	BehaviorTypeID Type = 0;
	string TypeName;
	string ModuleName = "Engine";
	util::UUID StableTypeID;
	uint32 SchemaVersion = 0;
	bool Enabled = true;
	BehaviorExecutionState State = BehaviorExecutionState::Unresolved;
	string Diagnostic;
	std::unordered_map<string, BehaviorPropertyValue> Properties;
};

struct BehaviorHandle final
{
	util::UUID InstanceID;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return InstanceID.IsValid();
	}
	[[nodiscard]] explicit operator bool() const noexcept
	{
		return this->IsValid();
	}
	[[nodiscard]] auto operator<=>(const BehaviorHandle &) const noexcept = default;
};

class ENGINE_API CObjectBehaviorComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<CObjectIdentityComponent>;

	explicit CObjectBehaviorComponent(world::ObjectHandle Owner) noexcept;
	CCOMPONENT_BODY(CObjectBehaviorComponent)

	[[nodiscard]] BehaviorHandle AddBehavior(BehaviorTypeID Type, string TypeName, uint32 SchemaVersion = 0);
	[[nodiscard]] BehaviorHandle AddBehavior(BehaviorInstance Behavior);
	[[nodiscard]] BehaviorHandle InsertBehavior(usize Index, BehaviorInstance Behavior);
	void RemoveBehavior(const util::UUID &InstanceID);
	void ReplaceBehavior(const BehaviorInstance &Behavior);
	void ReplaceBehaviors(std::vector<BehaviorInstance> Behaviors);
	void SetPropertiesAndSchema(const util::UUID &InstanceID, uint32 SchemaVersion,
								std::unordered_map<string, BehaviorPropertyValue> Properties);
	void SetExecutionState(const util::UUID &InstanceID, BehaviorExecutionState State, string Diagnostic = {});
	[[nodiscard]] std::optional<BehaviorInstance> FindBehavior(const util::UUID &InstanceID) const;
	[[nodiscard]] const std::vector<BehaviorInstance> &GetBehaviors() const noexcept;

  private:
	[[nodiscard]] BehaviorInstance *FindMutableBehavior(const util::UUID &InstanceID) noexcept;
	[[nodiscard]] static bool IsLegalTransition(BehaviorExecutionState From, BehaviorExecutionState To) noexcept;
	std::vector<BehaviorInstance> Behaviors;
};
} // namespace components
