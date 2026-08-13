#include "CObjectBehaviorComponent.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace components
{
util::UUID MakeBehaviorStableTypeID(const string_view ModuleName, const string_view TypeName) noexcept
{
	constexpr uint64 Prime = 1'099'511'628'211ULL;
	uint64 Left = 14'695'981'039'346'656'037ULL;
	uint64 Right = 7'806'847'184'892'571'929ULL;
	const auto Consume = [&](const string_view Text)
	{
		for (const char Character : Text)
		{
			const uint8 Byte = static_cast<uint8>(Character);
			Left = (Left ^ Byte) * Prime;
			Right = (Right ^ static_cast<uint8>(Byte + 0x9dU)) * Prime;
		}
	};
	Consume(ModuleName);
	Left = (Left ^ 0xffU) * Prime;
	Right = (Right ^ 0x5aU) * Prime;
	Consume(TypeName);
	return util::UUID(Left, Right);
}

CObjectBehaviorComponent::CObjectBehaviorComponent(const world::ObjectHandle Owner) noexcept : CObjectComponent(Owner)
{
}

BehaviorHandle CObjectBehaviorComponent::AddBehavior(const BehaviorTypeID Type, string TypeName, const uint32 SchemaVersion)
{
	return this->AddBehavior(
		{.Type = Type, .TypeName = std::move(TypeName), .SchemaVersion = SchemaVersion, .State = BehaviorExecutionState::Unresolved});
}

BehaviorHandle CObjectBehaviorComponent::AddBehavior(BehaviorInstance Behavior)
{
	return this->InsertBehavior(this->Behaviors.size(), std::move(Behavior));
}

BehaviorHandle CObjectBehaviorComponent::InsertBehavior(const usize Index, BehaviorInstance Behavior)
{
	if (Behavior.Type == 0)
		throw std::invalid_argument("Behavior type identity must be non-zero");
	if (Behavior.TypeName.empty())
		throw std::invalid_argument("Behavior type name cannot be empty");
	if (Behavior.ModuleName.empty())
		throw std::invalid_argument("Behavior module identity cannot be empty");
	if (!Behavior.StableTypeID.IsValid())
		Behavior.StableTypeID = MakeBehaviorStableTypeID(Behavior.ModuleName, Behavior.TypeName);
	if (!Behavior.InstanceID.IsValid())
		throw std::invalid_argument("Behavior instance identity must be valid");
	if (Index > this->Behaviors.size())
		throw std::out_of_range("Behavior insertion index exceeds the attached behavior count");
	if (this->FindBehavior(Behavior.InstanceID).has_value())
		throw std::invalid_argument("Behavior instance identity is already attached to this object");
	if (Behavior.State == BehaviorExecutionState::Constructed || Behavior.State == BehaviorExecutionState::Active)
		throw std::invalid_argument("Authored behavior insertion cannot attach a live runtime instance");
	const util::UUID InstanceID = Behavior.InstanceID;
	this->Behaviors.insert(this->Behaviors.begin() + static_cast<isize>(Index), std::move(Behavior));
	return {.InstanceID = InstanceID};
}

void CObjectBehaviorComponent::RemoveBehavior(const util::UUID &InstanceID)
{
	const auto Existing = std::find_if(this->Behaviors.begin(), this->Behaviors.end(),
									   [&InstanceID](const BehaviorInstance &Behavior) { return Behavior.InstanceID == InstanceID; });
	if (Existing == this->Behaviors.end())
		throw std::out_of_range("Behavior instance is not attached to this object");
	if (Existing->State == BehaviorExecutionState::Constructed || Existing->State == BehaviorExecutionState::Active)
		throw std::logic_error("An active behavior must be quiesced before it can be removed");
	this->Behaviors.erase(Existing);
}

void CObjectBehaviorComponent::ReplaceBehavior(const BehaviorInstance &Behavior)
{
	BehaviorInstance *Existing = this->FindMutableBehavior(Behavior.InstanceID);
	if (Existing == nullptr)
		throw std::out_of_range("Behavior instance is not attached to this object");
	if (Existing->Type != Behavior.Type || Existing->TypeName != Behavior.TypeName || Existing->ModuleName != Behavior.ModuleName ||
		Existing->StableTypeID != Behavior.StableTypeID)
		throw std::invalid_argument("Behavior editing cannot replace an instance's registered type identity");
	if (Existing->State == BehaviorExecutionState::Constructed || Existing->State == BehaviorExecutionState::Active)
		throw std::logic_error("An active behavior cannot be edited outside the runtime safe point");
	*Existing = Behavior;
}

void CObjectBehaviorComponent::ReplaceBehaviors(std::vector<BehaviorInstance> Behaviors)
{
	if (std::ranges::any_of(
			this->Behaviors, [](const BehaviorInstance &Behavior)
			{ return Behavior.State == BehaviorExecutionState::Constructed || Behavior.State == BehaviorExecutionState::Active; }))
	{
		throw std::logic_error("Active behaviors must be quiesced before replacing the component's attachment set");
	}
	std::vector<BehaviorInstance> Previous;
	Previous.swap(this->Behaviors);
	try
	{
		this->Behaviors.reserve(Behaviors.size());
		for (BehaviorInstance &Behavior : Behaviors)
			(void)this->InsertBehavior(this->Behaviors.size(), std::move(Behavior));
	}
	catch (...)
	{
		this->Behaviors = std::move(Previous);
		throw;
	}
}

void CObjectBehaviorComponent::SetPropertiesAndSchema(const util::UUID &InstanceID, const uint32 SchemaVersion,
													  std::unordered_map<string, BehaviorPropertyValue> Properties)
{
	BehaviorInstance *Existing = this->FindMutableBehavior(InstanceID);
	if (Existing == nullptr)
		throw std::out_of_range("Behavior instance is not attached to this object");
	if (Existing->State == BehaviorExecutionState::Active)
		throw std::logic_error("Active behavior properties may only change through the runtime safe point");
	Existing->SchemaVersion = SchemaVersion;
	Existing->Properties = std::move(Properties);
}

void CObjectBehaviorComponent::SetExecutionState(const util::UUID &InstanceID, const BehaviorExecutionState State, string Diagnostic)
{
	BehaviorInstance *Existing = this->FindMutableBehavior(InstanceID);
	if (Existing == nullptr)
		throw std::out_of_range("Behavior instance is not attached to this object");
	if (!CObjectBehaviorComponent::IsLegalTransition(Existing->State, State))
		throw std::logic_error("Behavior execution-state transition is not legal");
	if (State == BehaviorExecutionState::Failed && Diagnostic.empty())
		throw std::invalid_argument("A failed behavior state requires a diagnostic");
	Existing->State = State;
	Existing->Diagnostic = State == BehaviorExecutionState::Failed ? std::move(Diagnostic) : string{};
}

BehaviorInstance *CObjectBehaviorComponent::FindMutableBehavior(const util::UUID &InstanceID) noexcept
{
	const auto Existing = std::find_if(this->Behaviors.begin(), this->Behaviors.end(),
									   [&InstanceID](const BehaviorInstance &Behavior) { return Behavior.InstanceID == InstanceID; });
	return Existing == this->Behaviors.end() ? nullptr : &*Existing;
}

bool CObjectBehaviorComponent::IsLegalTransition(const BehaviorExecutionState From, const BehaviorExecutionState To) noexcept
{
	if (From == To || To == BehaviorExecutionState::Failed)
		return true;
	if (To == BehaviorExecutionState::Unresolved)
		return true;
	if (From == BehaviorExecutionState::Unresolved)
		return To == BehaviorExecutionState::Constructed || To == BehaviorExecutionState::Suspended;
	if (From == BehaviorExecutionState::Constructed)
		return To == BehaviorExecutionState::Active;
	if (From == BehaviorExecutionState::Active)
		return To == BehaviorExecutionState::Suspended;
	if (From == BehaviorExecutionState::Suspended)
		return To == BehaviorExecutionState::Constructed;
	if (From == BehaviorExecutionState::Failed)
		return To == BehaviorExecutionState::Unresolved;
	return false;
}

std::optional<BehaviorInstance> CObjectBehaviorComponent::FindBehavior(const util::UUID &InstanceID) const
{
	const BehaviorInstance *Behavior = const_cast<CObjectBehaviorComponent *>(this)->FindMutableBehavior(InstanceID);
	return Behavior == nullptr ? std::nullopt : std::optional<BehaviorInstance>(*Behavior);
}

const std::vector<BehaviorInstance> &CObjectBehaviorComponent::GetBehaviors() const noexcept
{
	return this->Behaviors;
}
} // namespace components
