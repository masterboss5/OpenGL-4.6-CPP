#include "ReflectionRegistry.h"

#include <bit>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace editor::reflection
{
void ReflectionRegistry::Register(TypeDescriptor Descriptor)
{
	if (Descriptor.Name.empty())
		throw std::invalid_argument("Reflected type name cannot be empty");
	if (Descriptor.DisplayName.empty())
		Descriptor.DisplayName = Descriptor.Name;
	if (Descriptor.ID == 0)
		Descriptor.ID = ReflectionRegistry::MakeTypeID(Descriptor.Name);

	for (const PropertyDescriptor &Property : Descriptor.Properties)
	{
		if (Property.Name.empty() || Property.DisplayName.empty() || Property.Category.empty())
			throw std::invalid_argument("Reflected properties require stable, display, and category names");
		if (!Property.Read)
			throw std::invalid_argument("Reflected properties require a read callback");
		if (!HasFlag(Property.Flags, PropertyFlags::ReadOnly) && !Property.Write)
			throw std::invalid_argument("Writable reflected properties require a write callback");
		if (!Property.EnumOptions.empty() && Property.Kind != PropertyKind::SignedInteger && Property.Kind != PropertyKind::UnsignedInteger)
		{
			throw std::invalid_argument("Reflected enum options require an integer property kind");
		}
		if (HasFlag(Property.Flags, PropertyFlags::Bitmask) &&
			(Property.Kind != PropertyKind::UnsignedInteger || Property.EnumOptions.empty()))
		{
			throw std::invalid_argument("Reflected bitmask properties require unsigned integer options");
		}
		std::unordered_set<uint64> EnumValues;
		for (const EnumPropertyOption &Option : Property.EnumOptions)
		{
			if (Option.DisplayName.empty() || !EnumValues.insert(Option.Value).second)
				throw std::invalid_argument("Reflected enum options require unique values and non-empty display names");
			if (HasFlag(Property.Flags, PropertyFlags::Bitmask) && !std::has_single_bit(Option.Value))
				throw std::invalid_argument("Reflected bitmask options must each identify exactly one bit");
		}
	}

	std::unique_lock Lock(this->Mutex);
	const auto Name = this->Names.find(Descriptor.Name);
	if (Name != this->Names.end() && Name->second != Descriptor.ID)
		throw std::logic_error("Reflected type name is already registered with another identity");
	const auto Existing = this->Types.find(Descriptor.ID);
	if (Existing != this->Types.end() && Existing->second.Name != Descriptor.Name)
		throw std::logic_error("Reflected type identity collision");
	this->Names.insert_or_assign(Descriptor.Name, Descriptor.ID);
	this->Types.insert_or_assign(Descriptor.ID, std::move(Descriptor));
	++this->Generation;
}

void ReflectionRegistry::Unregister(const ReflectionTypeID ID)
{
	std::unique_lock Lock(this->Mutex);
	const auto Existing = this->Types.find(ID);
	if (Existing == this->Types.end())
		return;
	this->Names.erase(Existing->second.Name);
	this->Types.erase(Existing);
	++this->Generation;
}

void ReflectionRegistry::Clear()
{
	std::unique_lock Lock(this->Mutex);
	this->Names.clear();
	this->Types.clear();
	++this->Generation;
}

std::optional<TypeDescriptor> ReflectionRegistry::Find(const ReflectionTypeID ID) const
{
	std::shared_lock Lock(this->Mutex);
	const auto Existing = this->Types.find(ID);
	return Existing == this->Types.end() ? std::nullopt : std::optional<TypeDescriptor>(Existing->second);
}

std::optional<TypeDescriptor> ReflectionRegistry::Find(const string_view Name) const
{
	std::shared_lock Lock(this->Mutex);
	const auto NameIterator = this->Names.find(string(Name));
	if (NameIterator == this->Names.end())
		return std::nullopt;
	const auto TypeIterator = this->Types.find(NameIterator->second);
	return TypeIterator == this->Types.end() ? std::nullopt : std::optional<TypeDescriptor>(TypeIterator->second);
}

std::vector<TypeDescriptor> ReflectionRegistry::Snapshot() const
{
	std::shared_lock Lock(this->Mutex);
	std::vector<TypeDescriptor> Result;
	Result.reserve(this->Types.size());
	for (const auto &[id, descriptor] : this->Types)
	{
		(void)id;
		Result.push_back(descriptor);
	}
	return Result;
}

uint64 ReflectionRegistry::GetGeneration() const
{
	std::shared_lock Lock(this->Mutex);
	return this->Generation;
}

ReflectionTypeID ReflectionRegistry::MakeTypeID(const string_view StableName) noexcept
{
	constexpr uint64 Offset = 14'695'981'039'346'656'037ULL;
	constexpr uint64 Prime = 1'099'511'628'211ULL;
	uint64 Hash = Offset;
	for (const auto Value : StableName)
	{
		Hash ^= static_cast<uint8>(Value);
		Hash *= Prime;
	}
	return Hash == 0 ? 1 : Hash;
}
} // namespace editor::reflection
