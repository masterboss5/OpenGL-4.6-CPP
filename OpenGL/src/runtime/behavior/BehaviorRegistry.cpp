#include "BehaviorRegistry.h"

#include "src/resource/asset/AssetManager.h"
#include "src/scene/Scene.h"
#include "src/scene/SceneCommandBuffer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <limits>
#include <mutex>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace runtime::behavior
{
namespace
{
[[nodiscard]] bool IsFinitePropertyValue(const components::BehaviorPropertyValue &Value)
{
	return std::visit(
		[](const auto &TypedValue)
		{
			using ValueType = std::remove_cvref_t<decltype(TypedValue)>;
			if constexpr (std::same_as<ValueType, float32> || std::same_as<ValueType, float64>)
				return std::isfinite(TypedValue);
			else if constexpr (std::same_as<ValueType, glm::vec2> || std::same_as<ValueType, glm::vec3> ||
							   std::same_as<ValueType, glm::vec4>)
			{
				for (glm::length_t Index = 0; Index < TypedValue.length(); ++Index)
					if (!std::isfinite(TypedValue[Index]))
						return false;
				return true;
			}
			else if constexpr (std::same_as<ValueType, glm::quat>)
				return std::isfinite(TypedValue.x) && std::isfinite(TypedValue.y) && std::isfinite(TypedValue.z) &&
					   std::isfinite(TypedValue.w);
			else
				return true;
		},
		Value);
}

void ValidatePropertyConstraint(const BehaviorPropertyDescriptor &Property, const components::BehaviorPropertyValue &Value)
{
	std::visit(
		[&Property](const auto &TypedValue)
		{
			using ValueType = std::remove_cvref_t<decltype(TypedValue)>;
			if constexpr (std::integral<ValueType> && !std::same_as<ValueType, bool> || std::floating_point<ValueType>)
			{
				const float64 NumericValue = static_cast<float64>(TypedValue);
				if (Property.NumericMinimum.has_value() && NumericValue < *Property.NumericMinimum)
					throw InvalidBehaviorDescriptorException("Behavior property '" + Property.Name + "' is below its minimum");
				if (Property.NumericMaximum.has_value() && NumericValue > *Property.NumericMaximum)
					throw InvalidBehaviorDescriptorException("Behavior property '" + Property.Name + "' exceeds its maximum");
			}
			else if constexpr (std::same_as<ValueType, string>)
			{
				if (TypedValue.size() > Property.MaximumStringBytes)
					throw InvalidBehaviorDescriptorException("Behavior property '" + Property.Name + "' exceeds its string budget");
			}
		},
		Value);
}
} // namespace

BehaviorExecutionContext::BehaviorExecutionContext(world::Scene &Scene, world::SceneCommandBuffer &Commands, resource::AssetManager &Assets,
												   const world::ObjectHandle Owner, const util::UUID InstanceID,
												   const std::unordered_map<string, components::BehaviorPropertyValue> &Properties) noexcept
	: Scene(&Scene), Commands(&Commands), Assets(&Assets), Owner(Owner), InstanceID(InstanceID), Properties(&Properties)
{
}

world::Scene &BehaviorExecutionContext::GetScene() const noexcept
{
	return *this->Scene;
}

world::SceneCommandBuffer &BehaviorExecutionContext::GetCommands() const noexcept
{
	return *this->Commands;
}

resource::AssetManager &BehaviorExecutionContext::GetAssets() const noexcept
{
	return *this->Assets;
}

world::ObjectHandle BehaviorExecutionContext::GetOwner() const noexcept
{
	return this->Owner;
}

const util::UUID &BehaviorExecutionContext::GetInstanceID() const noexcept
{
	return this->InstanceID;
}

const components::BehaviorPropertyValue *BehaviorExecutionContext::FindProperty(const string_view Name) const
{
	const auto Property = std::ranges::find_if(*this->Properties, [Name](const auto &Entry) { return Entry.first == Name; });
	return Property == this->Properties->end() ? nullptr : &Property->second;
}

void BehaviorRegistry::Register(BehaviorDescriptor Descriptor)
{
	Descriptor = BehaviorRegistry::Validate(std::move(Descriptor));
	std::unique_lock Lock(this->Mutex);
	if (this->Descriptors.contains(Descriptor.Type))
		throw DuplicateBehaviorDescriptorException("Behavior type identity is already registered");
	if (this->TypesByName.contains(Descriptor.Name))
		throw DuplicateBehaviorDescriptorException("Behavior type name '" + Descriptor.Name + "' is already registered");
	auto PreparedDescriptors = this->Descriptors;
	auto PreparedNames = this->TypesByName;
	PreparedNames.emplace(Descriptor.Name, Descriptor.Type);
	PreparedDescriptors.emplace(Descriptor.Type, std::move(Descriptor));
	this->Descriptors.swap(PreparedDescriptors);
	this->TypesByName.swap(PreparedNames);
	++this->Generation;
}

void BehaviorRegistry::ReplaceAll(const std::span<const BehaviorDescriptor> Descriptors)
{
	std::unordered_map<components::BehaviorTypeID, BehaviorDescriptor> Replacements;
	std::unordered_map<string, components::BehaviorTypeID> ReplacementNames;
	Replacements.reserve(Descriptors.size());
	ReplacementNames.reserve(Descriptors.size());
	for (const BehaviorDescriptor &Candidate : Descriptors)
	{
		BehaviorDescriptor Descriptor = BehaviorRegistry::Validate(Candidate);
		if (!Replacements.emplace(Descriptor.Type, Descriptor).second)
			throw DuplicateBehaviorDescriptorException("Replacement registry contains a duplicate behavior type identity");
		if (!ReplacementNames.emplace(Descriptor.Name, Descriptor.Type).second)
			throw DuplicateBehaviorDescriptorException("Replacement registry contains a duplicate behavior name '" + Descriptor.Name + "'");
	}

	std::unique_lock Lock(this->Mutex);
	for (const auto &[Type, Replacement] : Replacements)
	{
		const auto Previous = this->Descriptors.find(Type);
		if (Previous != this->Descriptors.end())
			BehaviorRegistry::ValidateReplacementCompatibility(Previous->second, Replacement);
	}
	this->Descriptors.swap(Replacements);
	this->TypesByName.swap(ReplacementNames);
	++this->Generation;
}

void BehaviorRegistry::RestoreSnapshot(const std::span<const BehaviorDescriptor> Descriptors)
{
	std::unordered_map<components::BehaviorTypeID, BehaviorDescriptor> Replacements;
	std::unordered_map<string, components::BehaviorTypeID> ReplacementNames;
	Replacements.reserve(Descriptors.size());
	ReplacementNames.reserve(Descriptors.size());
	for (const BehaviorDescriptor &Candidate : Descriptors)
	{
		BehaviorDescriptor Descriptor = BehaviorRegistry::Validate(Candidate);
		if (!Replacements.emplace(Descriptor.Type, Descriptor).second || !ReplacementNames.emplace(Descriptor.Name, Descriptor.Type).second)
		{
			throw DuplicateBehaviorDescriptorException("Behavior registry snapshot contains duplicate type identity or name");
		}
	}
	std::unique_lock Lock(this->Mutex);
	this->Descriptors.swap(Replacements);
	this->TypesByName.swap(ReplacementNames);
	++this->Generation;
}

void BehaviorRegistry::Clear()
{
	std::unique_lock Lock(this->Mutex);
	this->Descriptors.clear();
	this->TypesByName.clear();
	++this->Generation;
}

bool BehaviorRegistry::Contains(const components::BehaviorTypeID Type) const
{
	std::shared_lock Lock(this->Mutex);
	return this->Descriptors.contains(Type);
}

std::optional<BehaviorDescriptor> BehaviorRegistry::Find(const components::BehaviorTypeID Type) const
{
	std::shared_lock Lock(this->Mutex);
	const auto Descriptor = this->Descriptors.find(Type);
	return Descriptor == this->Descriptors.end() ? std::nullopt : std::optional<BehaviorDescriptor>(Descriptor->second);
}

std::vector<BehaviorDescriptor> BehaviorRegistry::Snapshot() const
{
	std::vector<BehaviorDescriptor> Result;
	this->SnapshotInto(Result);
	return Result;
}

void BehaviorRegistry::SnapshotInto(std::vector<BehaviorDescriptor> &Result) const
{
	std::shared_lock Lock(this->Mutex);
	Result.clear();
	if (Result.capacity() < this->Descriptors.size())
		Result.reserve(this->Descriptors.size());
	for (const auto &[Type, Descriptor] : this->Descriptors)
	{
		(void)Type;
		Result.push_back(Descriptor);
	}
	std::ranges::sort(Result, {}, &BehaviorDescriptor::Type);
}

uint64 BehaviorRegistry::GetGeneration() const
{
	std::shared_lock Lock(this->Mutex);
	return this->Generation;
}

void BehaviorRegistry::NormalizeProperties(const BehaviorDescriptor &Descriptor,
										   std::unordered_map<string, components::BehaviorPropertyValue> &Properties)
{
	for (const auto &[Name, Value] : Properties)
	{
		const auto Property = std::ranges::find(Descriptor.Properties, Name, &BehaviorPropertyDescriptor::Name);
		if (Property == Descriptor.Properties.end())
			throw InvalidBehaviorDescriptorException("Behavior instance contains unknown property '" + Name + "'");
		if (Property->DefaultValue.index() != Value.index())
			throw InvalidBehaviorDescriptorException("Behavior property '" + Name + "' has the wrong value type");
		if (!IsFinitePropertyValue(Value))
			throw InvalidBehaviorDescriptorException("Behavior property '" + Name + "' contains a non-finite numeric value");
		ValidatePropertyConstraint(*Property, Value);
	}
	for (const BehaviorPropertyDescriptor &Property : Descriptor.Properties)
		Properties.try_emplace(Property.Name, Property.DefaultValue);
}

BehaviorDescriptor BehaviorRegistry::Validate(BehaviorDescriptor Descriptor)
{
	if (Descriptor.Type == 0)
		throw InvalidBehaviorDescriptorException("Behavior descriptor type identity must be non-zero");
	if (Descriptor.Name.empty())
		throw InvalidBehaviorDescriptorException("Behavior descriptor name cannot be empty");
	if (Descriptor.ModuleName.empty())
		throw InvalidBehaviorDescriptorException("Behavior descriptor module identity cannot be empty");
	if (!Descriptor.StableTypeID.IsValid())
		Descriptor.StableTypeID = components::MakeBehaviorStableTypeID(Descriptor.ModuleName, Descriptor.Name);
	if (Descriptor.StateSize == 0)
		throw InvalidBehaviorDescriptorException("Behavior descriptor state size must be non-zero");
	if (Descriptor.StateAlignment == 0 || !std::has_single_bit(Descriptor.StateAlignment))
		throw InvalidBehaviorDescriptorException("Behavior descriptor state alignment must be a non-zero power of two");
	if (Descriptor.StateAlignment > 4'096U)
		throw InvalidBehaviorDescriptorException("Behavior descriptor state alignment exceeds the supported 4096-byte boundary");
	if (Descriptor.StateSize > std::numeric_limits<usize>::max() - (Descriptor.StateAlignment - 1U))
		throw InvalidBehaviorDescriptorException("Behavior descriptor state layout overflows addressable memory");
	if (Descriptor.Construct == nullptr || Descriptor.Destroy == nullptr)
		throw InvalidBehaviorDescriptorException("Behavior descriptor must provide construct and noexcept destroy callbacks");
	if ((Descriptor.SerializeState == nullptr) != (Descriptor.RestoreState == nullptr))
		throw InvalidBehaviorDescriptorException("Behavior descriptor must provide both state serialization and restoration callbacks");
	std::unordered_set<string> PropertyNames;
	for (const BehaviorPropertyDescriptor &Property : Descriptor.Properties)
	{
		if (Property.Name.empty() || !PropertyNames.emplace(Property.Name).second)
			throw InvalidBehaviorDescriptorException("Behavior descriptor property names must be non-empty and unique");
		if (!IsFinitePropertyValue(Property.DefaultValue))
			throw InvalidBehaviorDescriptorException("Behavior descriptor property defaults must contain only finite numeric values");
		if ((Property.NumericMinimum.has_value() && !std::isfinite(*Property.NumericMinimum)) ||
			(Property.NumericMaximum.has_value() && !std::isfinite(*Property.NumericMaximum)) ||
			(Property.NumericMinimum.has_value() && Property.NumericMaximum.has_value() &&
			 *Property.NumericMinimum > *Property.NumericMaximum))
		{
			throw InvalidBehaviorDescriptorException("Behavior property numeric constraints are invalid");
		}
		ValidatePropertyConstraint(Property, Property.DefaultValue);
	}
	std::ranges::sort(Descriptor.Properties, {}, &BehaviorPropertyDescriptor::Name);
	return Descriptor;
}

void BehaviorRegistry::ValidateReplacementCompatibility(const BehaviorDescriptor &Previous, const BehaviorDescriptor &Replacement)
{
	if (Previous.Name != Replacement.Name || Previous.ModuleName != Replacement.ModuleName ||
		Previous.StableTypeID != Replacement.StableTypeID)
		throw InvalidBehaviorDescriptorException("Behavior replacement cannot change the name bound to an existing type identity");
	if (Replacement.SchemaVersion < Previous.SchemaVersion)
		throw InvalidBehaviorDescriptorException("Behavior replacement schema version cannot move backwards");
	if (Replacement.SchemaVersion != Previous.SchemaVersion)
	{
		if (Replacement.MigrateProperties == nullptr)
			throw InvalidBehaviorDescriptorException("A behavior schema-version increase requires a property migration callback");
		return;
	}
	if (Previous.StateSize != Replacement.StateSize || Previous.StateAlignment != Replacement.StateAlignment ||
		Previous.Properties.size() != Replacement.Properties.size())
	{
		throw InvalidBehaviorDescriptorException("Behavior layout changed without increasing its schema version");
	}
	for (usize Index = 0; Index < Previous.Properties.size(); ++Index)
	{
		const BehaviorPropertyDescriptor &OldProperty = Previous.Properties[Index];
		const BehaviorPropertyDescriptor &NewProperty = Replacement.Properties[Index];
		if (OldProperty.Name != NewProperty.Name || OldProperty.DefaultValue.index() != NewProperty.DefaultValue.index() ||
			OldProperty.ReadOnly != NewProperty.ReadOnly || OldProperty.NumericMinimum != NewProperty.NumericMinimum ||
			OldProperty.NumericMaximum != NewProperty.NumericMaximum || OldProperty.MaximumStringBytes != NewProperty.MaximumStringBytes)
		{
			throw InvalidBehaviorDescriptorException("Behavior property schema changed without increasing its schema version");
		}
	}
}
} // namespace runtime::behavior
