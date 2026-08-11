#pragma once

#include "src/component/object/CObjectComponent.h"
#include "src/types.h"
#include "src/util/UUID.h"

#include <span>
#include <vector>

namespace components
{
enum class ObjectMobility : uint8
{
	Static,
	Stationary,
	Movable
};

class ENGINE_API CObjectIdentityComponent final : public CObjectComponent
{
  public:
	using Dependencies = TypeList<>;

	explicit CObjectIdentityComponent(world::ObjectHandle Owner, string Name = "Object",
									  util::UUID PersistentID = util::UUID::GenerateRandomUUID());
	CCOMPONENT_BODY(CObjectIdentityComponent)

	[[nodiscard]] const util::UUID &GetPersistentID() const noexcept;
	[[nodiscard]] const string &GetName() const noexcept;
	void SetName(string Name);
	[[nodiscard]] std::span<const string> GetTags() const noexcept;
	void SetTags(std::vector<string> Tags);
	void AddTag(string Tag);
	[[nodiscard]] bool RemoveTag(string_view Tag) noexcept;
	[[nodiscard]] bool HasTag(string_view Tag) const noexcept;
	[[nodiscard]] ObjectMobility GetMobility() const noexcept;
	void SetMobility(ObjectMobility Mobility);
	[[nodiscard]] bool IsEditorVisible() const noexcept;
	void SetEditorVisible(bool Visible) noexcept;
	[[nodiscard]] bool IsLocked() const noexcept;
	void SetLocked(bool Locked) noexcept;

  private:
	util::UUID PersistentID;
	string Name;
	std::vector<string> Tags;
	ObjectMobility Mobility = ObjectMobility::Movable;
	bool EditorVisible = true;
	bool Locked = false;
};
} // namespace components
