#include "CObjectIdentityComponent.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace components
{
CObjectIdentityComponent::CObjectIdentityComponent(const world::ObjectHandle Owner, string Name, const util::UUID PersistentID)
	: CObjectComponent(Owner), PersistentID(PersistentID), Name(std::move(Name))
{
	if (!this->PersistentID.IsValid())
		throw std::invalid_argument("Object persistent identity must be valid");
	if (this->Name.empty())
		throw std::invalid_argument("Object name cannot be empty");
}

const util::UUID &CObjectIdentityComponent::GetPersistentID() const noexcept
{
	return this->PersistentID;
}

const string &CObjectIdentityComponent::GetName() const noexcept
{
	return this->Name;
}

void CObjectIdentityComponent::SetName(string Name)
{
	if (Name.empty())
		throw std::invalid_argument("Object name cannot be empty");
	this->Name = std::move(Name);
}

std::span<const string> CObjectIdentityComponent::GetTags() const noexcept
{
	return this->Tags;
}

void CObjectIdentityComponent::SetTags(std::vector<string> Tags)
{
	std::unordered_set<string> Unique;
	Unique.reserve(Tags.size());
	for (const string &Tag : Tags)
	{
		if (Tag.empty())
			throw std::invalid_argument("Object tags cannot contain an empty value");
		if (!Unique.emplace(Tag).second)
			throw std::invalid_argument("Object tags cannot contain duplicate values");
	}
	this->Tags = std::move(Tags);
}

void CObjectIdentityComponent::AddTag(string Tag)
{
	if (Tag.empty())
		throw std::invalid_argument("Object tag cannot be empty");
	if (this->HasTag(Tag))
		throw std::invalid_argument("Object tag is already attached");
	this->Tags.push_back(std::move(Tag));
}

bool CObjectIdentityComponent::RemoveTag(const string_view Tag) noexcept
{
	const auto Found = std::ranges::find(this->Tags, Tag);
	if (Found == this->Tags.end())
		return false;
	this->Tags.erase(Found);
	return true;
}

bool CObjectIdentityComponent::HasTag(const string_view Tag) const noexcept
{
	return std::ranges::find(this->Tags, Tag) != this->Tags.end();
}

ObjectMobility CObjectIdentityComponent::GetMobility() const noexcept
{
	return this->Mobility;
}

void CObjectIdentityComponent::SetMobility(const ObjectMobility Mobility)
{
	if (static_cast<uint32>(Mobility) > static_cast<uint32>(ObjectMobility::Movable))
		throw std::invalid_argument("Object mobility is invalid");
	this->Mobility = Mobility;
}

bool CObjectIdentityComponent::IsEditorVisible() const noexcept
{
	return this->EditorVisible;
}

void CObjectIdentityComponent::SetEditorVisible(const bool Visible) noexcept
{
	this->EditorVisible = Visible;
}

bool CObjectIdentityComponent::IsLocked() const noexcept
{
	return this->Locked;
}

void CObjectIdentityComponent::SetLocked(const bool Locked) noexcept
{
	this->Locked = Locked;
}
} // namespace components
