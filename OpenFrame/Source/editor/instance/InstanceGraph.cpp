#include "InstanceGraph.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_set>

namespace editor::instance
{
namespace
{
[[nodiscard]] uint64 AdvanceRevision(const uint64 Revision) noexcept
{
	return Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1;
}

void ValidateAndNormalizeProperty(const InstanceTypeDescriptor &Descriptor, const string_view Name, InstancePropertyValue &Value)
{
	const auto Definition = Descriptor.DefaultProperties.find(string(Name));
	const bool DynamicBehaviorProperty = Descriptor.ClassID == class_ids::Script && Name.starts_with("Behavior.");
	if (Definition == Descriptor.DefaultProperties.end() && !DynamicBehaviorProperty)
		throw std::invalid_argument("Instance property is not declared by its class: " + string(Name));
	if (Definition != Descriptor.DefaultProperties.end() && Definition->second.index() != Value.index())
		throw std::invalid_argument("Instance property value has the wrong type: " + string(Name));
	if (DynamicBehaviorProperty && std::holds_alternative<InstanceAssetReference>(Value))
		throw std::invalid_argument("Script behavior properties cannot store asset references");
	const auto Schema = std::ranges::find(Descriptor.Properties, Name, &InstancePropertyDescriptor::Name);
	if (!DynamicBehaviorProperty && Schema == Descriptor.Properties.end())
		throw std::logic_error("Instance class property schema is incomplete: " + string(Name));
	std::visit(
		[&Name](auto &TypedValue)
		{
			using ValueType = std::decay_t<decltype(TypedValue)>;
			if constexpr (std::same_as<ValueType, float32> || std::same_as<ValueType, float64>)
			{
				if (!std::isfinite(TypedValue))
					throw std::invalid_argument("Instance property must be finite: " + string(Name));
			}
			else if constexpr (std::same_as<ValueType, glm::vec2> || std::same_as<ValueType, glm::vec3> ||
							   std::same_as<ValueType, glm::vec4>)
			{
				for (uint32 Index = 0; Index < static_cast<uint32>(TypedValue.length()); ++Index)
				{
					if (!std::isfinite(TypedValue[Index]))
						throw std::invalid_argument("Instance vector property must be finite: " + string(Name));
				}
			}
			else if constexpr (std::same_as<ValueType, glm::quat>)
			{
				const float32 Length = glm::length(TypedValue);
				if (!std::isfinite(Length) || Length <= 1.0e-6F)
					throw std::invalid_argument("Instance rotation property must be normalizable: " + string(Name));
				TypedValue = glm::normalize(TypedValue);
			}
			else if constexpr (std::same_as<ValueType, string>)
			{
				if (TypedValue.size() > 64U * 1024U)
					throw std::length_error("Instance string property exceeds the supported size: " + string(Name));
			}
			else if constexpr (std::same_as<ValueType, InstanceAssetReference>)
			{
				if (!TypedValue.ID.empty() && TypedValue.Type == resource::AssetType::Count)
					throw std::invalid_argument("Instance asset property has no concrete asset type: " + string(Name));
			}
		},
		Value);
	if (Schema != Descriptor.Properties.end())
	{
		const auto NumericValue = std::visit(
			[](const auto &TypedValue) -> std::optional<float64>
			{
				using ValueType = std::decay_t<decltype(TypedValue)>;
				if constexpr (std::integral<ValueType> && !std::same_as<ValueType, bool>)
					return static_cast<float64>(TypedValue);
				else if constexpr (std::floating_point<ValueType>)
					return static_cast<float64>(TypedValue);
				else
					return std::nullopt;
			},
			Value);
		if (Schema->Minimum.has_value() && NumericValue.has_value() && *NumericValue < *Schema->Minimum)
			throw std::out_of_range("Instance property is below its declared minimum: " + string(Name));
		if (Schema->Maximum.has_value() && NumericValue.has_value() && *NumericValue > *Schema->Maximum)
			throw std::out_of_range("Instance property exceeds its declared maximum: " + string(Name));
		if (!Schema->Choices.empty())
		{
			string Choice;
			if (const auto *Text = std::get_if<string>(&Value))
				Choice = *Text;
			else if (const auto *Unsigned = std::get_if<uint64>(&Value))
				Choice = std::to_string(*Unsigned);
			else if (const auto *Unsigned32 = std::get_if<uint32>(&Value))
				Choice = std::to_string(*Unsigned32);
			if (Choice.empty() || std::ranges::find(Schema->Choices, Choice) == Schema->Choices.end())
				throw std::invalid_argument("Instance property is not one of its declared choices: " + string(Name));
		}
		if (const auto *Asset = std::get_if<InstanceAssetReference>(&Value))
		{
			const auto *DefaultAsset = std::get_if<InstanceAssetReference>(&Schema->DefaultValue);
			if (DefaultAsset == nullptr || Asset->Type != DefaultAsset->Type)
				throw std::invalid_argument("Instance asset property has the wrong asset type: " + string(Name));
		}
	}
}

void ValidateRecordSemantics(const InstanceTypeDescriptor &Descriptor, const InstancePropertyMap &Properties)
{
	const auto Number = [&Properties](const string_view Name) -> std::optional<float64>
	{
		const auto Found = Properties.find(string(Name));
		if (Found == Properties.end())
			return std::nullopt;
		return std::visit(
			[](const auto &Value) -> std::optional<float64>
			{
				using ValueType = std::decay_t<decltype(Value)>;
				if constexpr ((std::integral<ValueType> && !std::same_as<ValueType, bool>) || std::floating_point<ValueType>)
					return static_cast<float64>(Value);
				else
					return std::nullopt;
			},
			Found->second);
	};
	const std::optional<float64> Near = Number("NearPlane");
	const std::optional<float64> Far = Number("FarPlane");
	if (Near.has_value() && Far.has_value() && *Near >= *Far)
		throw std::invalid_argument("Camera Near Plane must be smaller than Far Plane");
	const std::optional<float64> Inner = Number("InnerConeDegrees");
	const std::optional<float64> Outer = Number("OuterConeDegrees");
	if (Inner.has_value() && Outer.has_value() && *Inner > *Outer)
		throw std::invalid_argument("SpotLight Inner Cone must not exceed Outer Cone");
	for (const string_view Name : {string_view("Scale"), string_view("PivotScale")})
	{
		const auto Found = Properties.find(string(Name));
		if (Found == Properties.end())
			continue;
		const glm::vec3 *Scale = std::get_if<glm::vec3>(&Found->second);
		if (Scale == nullptr || Scale->x < 1.0e-4F || Scale->y < 1.0e-4F || Scale->z < 1.0e-4F)
			throw std::out_of_range("Instance transform scale is below the supported minimum");
	}
	(void)Descriptor;
}
} // namespace

InstanceGraph::InstanceGraph(const InstanceTypeRegistry &Types, const bool CreateServices)
	: Types(&Types), OwnerThread(std::this_thread::get_id())
{
	if (CreateServices)
		this->CreateServices();
}

util::UUID InstanceGraph::Create(const InstanceClassID &ClassID, const util::UUID &Parent, string Name, const util::UUID ID,
								 InstancePropertyMap InitialProperties)
{
	this->AssertOwnerThread();
	const std::shared_ptr<const InstanceTypeDescriptor> Descriptor = this->Types->Find(ClassID);
	if (Descriptor == nullptr)
		throw std::invalid_argument("Cannot create an unregistered instance class");
	if (!Descriptor->Creatable)
		throw std::invalid_argument("Instance class is not directly creatable");
	if (!ID.IsValid())
		throw std::invalid_argument("Instance requires a valid persistent identity");

	std::unique_lock Lock(this->Mutex);
	if (this->Records.contains(ID))
		throw std::invalid_argument("Instance identity already exists");
	if (Parent.IsValid() && !this->Records.contains(Parent))
		throw std::out_of_range("Instance parent does not exist");
	if (Name.empty())
		Name = Descriptor->DisplayName;
	Name = this->MakeUniqueNameUnlocked(Parent, std::move(Name));

	auto &Siblings = Parent.IsValid() ? this->Records.at(Parent).Children : this->Roots;
	const uint32 Order = static_cast<uint32>(Siblings.size());
	InstancePropertyMap Properties = Descriptor->DefaultProperties;
	for (auto &[PropertyName, Value] : InitialProperties)
	{
		ValidateAndNormalizeProperty(*Descriptor, PropertyName, Value);
		Properties.insert_or_assign(std::move(PropertyName), std::move(Value));
	}
	ValidateRecordSemantics(*Descriptor, Properties);
	InstanceRecord Record{.ID = ID,
						  .ClassID = Descriptor->ClassID,
						  .ClassName = Descriptor->ClassName,
						  .Name = std::move(Name),
						  .Parent = Parent,
						  .Properties = std::move(Properties),
						  .SiblingOrder = Order};
	this->Records.emplace(ID, std::move(Record));
	try
	{
		Siblings.push_back(ID);
	}
	catch (...)
	{
		this->Records.erase(ID);
		throw;
	}
	this->Touch();
	return ID;
}

void InstanceGraph::Destroy(const util::UUID &ID)
{
	this->AssertOwnerThread();
	std::unique_lock Lock(this->Mutex);
	const auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	if (Iterator->second.Protected)
		throw std::logic_error("Protected service instances cannot be destroyed");

	std::vector<util::UUID> Pending{ID};
	for (usize Index = 0; Index < Pending.size(); ++Index)
	{
		const auto Current = this->Records.find(Pending[Index]);
		if (Current != this->Records.end())
			Pending.insert(Pending.end(), Current->second.Children.begin(), Current->second.Children.end());
	}
	auto &Siblings = Iterator->second.Parent.IsValid() ? this->Records.at(Iterator->second.Parent).Children : this->Roots;
	std::erase(Siblings, ID);
	const util::UUID Parent = Iterator->second.Parent;
	for (auto Reverse = Pending.rbegin(); Reverse != Pending.rend(); ++Reverse)
		this->Records.erase(*Reverse);
	this->NormalizeSiblingOrderUnlocked(Parent);
	this->Touch();
}

void InstanceGraph::Rename(const util::UUID &ID, string Name)
{
	this->AssertOwnerThread();
	if (Name.empty())
		throw std::invalid_argument("Instance name cannot be empty");
	std::unique_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	if (Iterator->second.Protected)
		throw std::logic_error("Protected service instances cannot be renamed");
	Iterator->second.Name = this->MakeUniqueNameUnlocked(Iterator->second.Parent, std::move(Name));
	this->Touch();
}

void InstanceGraph::SetClass(const util::UUID &ID, const InstanceClassID &ClassID)
{
	this->AssertOwnerThread();
	const std::shared_ptr<const InstanceTypeDescriptor> Descriptor = this->Types->Find(ClassID);
	if (Descriptor == nullptr || Descriptor->Service)
		throw std::invalid_argument("Instance class cannot be assigned");
	std::unique_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	if (Iterator->second.Protected)
		throw std::logic_error("Protected service instance class cannot be changed");
	InstancePropertyMap Replacement = Descriptor->DefaultProperties;
	for (const auto &[Name, Value] : Iterator->second.Properties)
	{
		const auto NewProperty = Descriptor->DefaultProperties.find(Name);
		if (NewProperty != Descriptor->DefaultProperties.end() && NewProperty->second.index() == Value.index())
			Replacement.insert_or_assign(Name, Value);
	}
	ValidateRecordSemantics(*Descriptor, Replacement);
	Iterator->second.ClassID = Descriptor->ClassID;
	Iterator->second.ClassName = Descriptor->ClassName;
	Iterator->second.Properties = std::move(Replacement);
	this->Touch();
}

void InstanceGraph::Reparent(const util::UUID &ID, const util::UUID &Parent, const uint32 SiblingOrder)
{
	this->AssertOwnerThread();
	std::unique_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	if (Iterator->second.Protected)
		throw std::logic_error("Protected service instances cannot be reparented");
	if (Parent == ID || (Parent.IsValid() && this->IsDescendantUnlocked(Parent, ID)))
		throw std::invalid_argument("Instance parenting would create a hierarchy cycle");
	if (Parent.IsValid() && !this->Records.contains(Parent))
		throw std::out_of_range("Instance parent does not exist");

	const util::UUID PreviousParent = Iterator->second.Parent;
	auto &PreviousSiblings = PreviousParent.IsValid() ? this->Records.at(PreviousParent).Children : this->Roots;
	std::erase(PreviousSiblings, ID);
	this->NormalizeSiblingOrderUnlocked(PreviousParent);
	auto &NewSiblings = Parent.IsValid() ? this->Records.at(Parent).Children : this->Roots;
	const usize InsertIndex = std::min<usize>(SiblingOrder, NewSiblings.size());
	NewSiblings.insert(NewSiblings.begin() + static_cast<isize>(InsertIndex), ID);
	Iterator = this->Records.find(ID);
	Iterator->second.Parent = Parent;
	this->NormalizeSiblingOrderUnlocked(Parent);
	this->Touch();
}

void InstanceGraph::SetEnabled(const util::UUID &ID, const bool Enabled)
{
	this->AssertOwnerThread();
	std::unique_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	Iterator->second.Enabled = Enabled;
	this->Touch();
}

void InstanceGraph::SetProperty(const util::UUID &ID, string Name, InstancePropertyValue Value)
{
	this->AssertOwnerThread();
	if (Name.empty())
		throw std::invalid_argument("Instance property name cannot be empty");
	std::unique_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	const std::shared_ptr<const InstanceTypeDescriptor> Descriptor = this->Types->Find(Iterator->second.ClassID);
	if (Descriptor == nullptr)
		throw std::logic_error("Instance class is no longer registered");
	ValidateAndNormalizeProperty(*Descriptor, Name, Value);
	InstancePropertyMap Candidate = Iterator->second.Properties;
	Candidate.insert_or_assign(Name, Value);
	ValidateRecordSemantics(*Descriptor, Candidate);
	Iterator->second.Properties = std::move(Candidate);
	this->Touch();
}

void InstanceGraph::RemoveProperty(const util::UUID &ID, const string_view Name)
{
	this->AssertOwnerThread();
	std::unique_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	const std::shared_ptr<const InstanceTypeDescriptor> Descriptor = this->Types->Find(Iterator->second.ClassID);
	if (Descriptor == nullptr)
		throw std::logic_error("Instance class is no longer registered");
	if (Descriptor->DefaultProperties.contains(string(Name)))
		throw std::logic_error("Declared instance properties cannot be removed");
	if (Iterator->second.ClassID != class_ids::Script || !Name.starts_with("Behavior."))
		throw std::logic_error("Only dynamic Script behavior properties can be removed");
	if (Iterator->second.Properties.erase(string(Name)) == 0)
		throw std::out_of_range("Instance property does not exist");
	this->Touch();
}

void InstanceGraph::ApplyModelPivotDelta(const util::UUID &Model, const glm::vec3 &Translation, const glm::quat &Rotation)
{
	this->AssertOwnerThread();
	if (!std::isfinite(Translation.x) || !std::isfinite(Translation.y) || !std::isfinite(Translation.z) || !std::isfinite(Rotation.w) ||
		!std::isfinite(Rotation.x) || !std::isfinite(Rotation.y) || !std::isfinite(Rotation.z))
	{
		throw std::invalid_argument("Model pivot delta must be finite");
	}
	const float32 RotationLength = glm::length(Rotation);
	if (!std::isfinite(RotationLength) || RotationLength <= 1.0e-6F)
		throw std::invalid_argument("Model pivot rotation must be normalizable");
	const glm::quat NormalizedRotation = glm::normalize(Rotation);
	std::unique_lock Lock(this->Mutex);
	auto ModelIterator = this->Records.find(Model);
	if (ModelIterator == this->Records.end() || ModelIterator->second.ClassID != class_ids::Model)
		throw std::invalid_argument("Model pivot operation requires a Model instance");
	const glm::vec3 PreviousPivot = std::get<glm::vec3>(ModelIterator->second.Properties.at("PivotPosition"));
	const glm::quat PreviousRotation = std::get<glm::quat>(ModelIterator->second.Properties.at("PivotRotation"));
	std::vector<util::UUID> Pending = ModelIterator->second.Children;
	while (!Pending.empty())
	{
		const util::UUID ID = Pending.back();
		Pending.pop_back();
		auto &Record = this->Records.at(ID);
		Pending.insert(Pending.end(), Record.Children.begin(), Record.Children.end());
		if (Record.ClassID != class_ids::Part && Record.ClassID != class_ids::MeshPart)
			continue;
		auto Position = Record.Properties.find("Position");
		auto PartRotation = Record.Properties.find("Rotation");
		if (Position == Record.Properties.end() || PartRotation == Record.Properties.end())
			continue;
		const glm::vec3 Relative = std::get<glm::vec3>(Position->second) - PreviousPivot;
		Position->second = PreviousPivot + Translation + NormalizedRotation * Relative;
		PartRotation->second = glm::normalize(NormalizedRotation * std::get<glm::quat>(PartRotation->second));
	}
	ModelIterator->second.Properties.at("PivotPosition") = PreviousPivot + Translation;
	ModelIterator->second.Properties.at("PivotRotation") = glm::normalize(NormalizedRotation * PreviousRotation);
	this->Touch();
}

void InstanceGraph::SetWorldTransform(const util::UUID &ID, const glm::vec3 &Position, const glm::quat &Rotation, const glm::vec3 &Scale)
{
	this->AssertOwnerThread();
	const auto IsFiniteVector = [](const glm::vec3 &Value)
	{ return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z); };
	if (!IsFiniteVector(Position) || !IsFiniteVector(Scale) || !std::isfinite(Rotation.w) || !std::isfinite(Rotation.x) ||
		!std::isfinite(Rotation.y) || !std::isfinite(Rotation.z))
	{
		throw std::invalid_argument("Instance transform must be finite");
	}
	if (Scale.x <= 1.0e-4F || Scale.y <= 1.0e-4F || Scale.z <= 1.0e-4F)
		throw std::invalid_argument("Instance transform scale must remain positive and non-singular");
	const float32 RotationLength = glm::length(Rotation);
	if (!std::isfinite(RotationLength) || RotationLength <= 1.0e-6F)
		throw std::invalid_argument("Instance transform rotation must be normalizable");
	const glm::quat NormalizedRotation = glm::normalize(Rotation);

	std::unique_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	InstanceRecord &Record = Iterator->second;
	if (Record.ClassID == class_ids::Model)
	{
		const glm::vec3 PreviousPosition = std::get<glm::vec3>(Record.Properties.at("PivotPosition"));
		const glm::quat PreviousRotation = glm::normalize(std::get<glm::quat>(Record.Properties.at("PivotRotation")));
		const glm::vec3 PreviousScale = std::get<glm::vec3>(Record.Properties.at("PivotScale"));
		const glm::quat RotationDelta = glm::normalize(NormalizedRotation * glm::inverse(PreviousRotation));
		const glm::vec3 ScaleRatio = Scale / PreviousScale;
		std::vector<util::UUID> Pending = Record.Children;
		while (!Pending.empty())
		{
			const util::UUID ChildID = Pending.back();
			Pending.pop_back();
			InstanceRecord &Child = this->Records.at(ChildID);
			Pending.insert(Pending.end(), Child.Children.begin(), Child.Children.end());
			if (Child.ClassID != class_ids::Part && Child.ClassID != class_ids::MeshPart)
				continue;
			auto ChildPosition = Child.Properties.find("Position");
			auto ChildRotation = Child.Properties.find("Rotation");
			auto ChildScale = Child.Properties.find("Scale");
			if (ChildPosition == Child.Properties.end() || ChildRotation == Child.Properties.end() || ChildScale == Child.Properties.end())
				continue;
			const glm::vec3 PreviousRelative =
				glm::inverse(PreviousRotation) * (std::get<glm::vec3>(ChildPosition->second) - PreviousPosition);
			ChildPosition->second = Position + NormalizedRotation * (ScaleRatio * PreviousRelative);
			ChildRotation->second = glm::normalize(RotationDelta * std::get<glm::quat>(ChildRotation->second));
			ChildScale->second = std::get<glm::vec3>(ChildScale->second) * ScaleRatio;
		}
		Record.Properties.at("PivotPosition") = Position;
		Record.Properties.at("PivotRotation") = NormalizedRotation;
		Record.Properties.at("PivotScale") = Scale;
	}
	else
	{
		const std::shared_ptr<const InstanceTypeDescriptor> Descriptor = this->Types->Find(Record.ClassID);
		if (Descriptor == nullptr)
			throw std::logic_error("Instance class is no longer registered");
		if (!Descriptor->TransformCapabilities.SupportsAnyTransform())
			throw std::invalid_argument("Instance class does not expose a world transform");
		if (Descriptor->TransformCapabilities.Translation)
		{
			if (!Record.Properties.contains("Position"))
				throw std::logic_error("Translatable instance class has no Position property");
			Record.Properties.at("Position") = Position;
		}
		if (Descriptor->TransformCapabilities.Rotation)
		{
			if (!Record.Properties.contains("Rotation"))
				throw std::logic_error("Rotatable instance class has no Rotation property");
			Record.Properties.at("Rotation") = NormalizedRotation;
		}
		if (Descriptor->TransformCapabilities.Scale)
		{
			if (!Record.Properties.contains("Scale"))
				throw std::logic_error("Scalable instance class has no Scale property");
			Record.Properties.at("Scale") = Scale;
		}
	}
	this->Touch();
}

bool InstanceGraph::Contains(const util::UUID &ID) const
{
	std::shared_lock Lock(this->Mutex);
	return this->Records.contains(ID);
}

InstanceRecord InstanceGraph::Get(const util::UUID &ID) const
{
	std::shared_lock Lock(this->Mutex);
	const auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	return Iterator->second;
}

InstanceActivation InstanceGraph::GetActivation(const util::UUID &ID) const
{
	std::shared_lock Lock(this->Mutex);
	const auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end())
		throw std::out_of_range("Instance does not exist");
	const InstanceRecord &Record = Iterator->second;
	const std::shared_ptr<const InstanceTypeDescriptor> Descriptor = this->Types->Find(Record.ClassID);
	if (Descriptor == nullptr)
		return {.State = InstanceActivationState::Inactive, .Diagnostic = "Instance class is not registered"};
	if (Descriptor->Availability == InstanceAvailability::Unavailable)
		return {.State = InstanceActivationState::Unavailable, .Diagnostic = "This instance class is not implemented yet"};
	if (!Record.Enabled)
		return {.State = InstanceActivationState::Inactive, .Diagnostic = "Instance is disabled"};
	if (!Descriptor->AllowedServiceClasses.empty())
	{
		const InstanceRecord *Root = &Record;
		while (Root->Parent.IsValid())
		{
			const auto Parent = this->Records.find(Root->Parent);
			if (Parent == this->Records.end())
				return {.State = InstanceActivationState::Inactive, .Diagnostic = "Instance hierarchy is incomplete"};
			Root = &Parent->second;
		}
		if (std::ranges::find(Descriptor->AllowedServiceClasses, Root->ClassID) == Descriptor->AllowedServiceClasses.end())
		{
			return {.State = InstanceActivationState::Inactive, .Diagnostic = "Instance is outside a compatible service hierarchy"};
		}
	}
	if (!Descriptor->ExactParentClasses.empty())
	{
		if (!Record.Parent.IsValid())
			return {.State = InstanceActivationState::Inactive, .Diagnostic = "Instance requires a compatible immediate parent"};
		const auto Parent = this->Records.find(Record.Parent);
		if (Parent == this->Records.end() ||
			std::ranges::find(Descriptor->ExactParentClasses, Parent->second.ClassID) == Descriptor->ExactParentClasses.end())
		{
			return {.State = InstanceActivationState::Inactive,
					.Diagnostic = "Immediate parent is incompatible with " + Descriptor->DisplayName};
		}
	}
	if (Record.ClassID == class_ids::AnimationTrack)
	{
		const auto Clip = Record.Properties.find("Clip");
		if (Clip == Record.Properties.end() || !std::holds_alternative<InstanceAssetReference>(Clip->second) ||
			std::get<InstanceAssetReference>(Clip->second).ID.empty())
		{
			return {.State = InstanceActivationState::Inactive, .Diagnostic = "AnimationTrack requires exactly one AnimationClip asset"};
		}
	}
	if (Record.ClassID == class_ids::Script)
	{
		const auto StableType = Record.Properties.find("StableTypeID");
		if (StableType == Record.Properties.end() || !std::holds_alternative<util::UUID>(StableType->second) ||
			!std::get<util::UUID>(StableType->second).IsValid())
		{
			return {.State = InstanceActivationState::Inactive, .Diagnostic = "Script requires a registered behavior type"};
		}
	}
	return {.State = InstanceActivationState::Active};
}

InstanceGraphSnapshot InstanceGraph::Snapshot() const
{
	std::shared_lock Lock(this->Mutex);
	InstanceGraphSnapshot Result{.Revision = this->Revision};
	Result.Instances.reserve(this->Records.size());
	const auto Append = [this, &Result](const auto &Self, const util::UUID &ID) -> void
	{
		const auto Iterator = this->Records.find(ID);
		if (Iterator == this->Records.end())
			return;
		Result.Instances.push_back(Iterator->second);
		for (const util::UUID &Child : Iterator->second.Children)
			Self(Self, Child);
	};
	for (const util::UUID &Root : this->Roots)
		Append(Append, Root);
	return Result;
}

void InstanceGraph::LoadSnapshot(const InstanceGraphSnapshot &Snapshot)
{
	this->AssertOwnerThread();
	if (Snapshot.Revision == 0)
		throw std::invalid_argument("Instance graph snapshot revision must be nonzero");
	std::unordered_map<util::UUID, InstanceRecord> Replacement;
	Replacement.reserve(Snapshot.Instances.size());
	for (const InstanceRecord &Source : Snapshot.Instances)
	{
		const std::shared_ptr<const InstanceTypeDescriptor> Descriptor = this->Types->Find(Source.ClassID);
		if (!Source.ID.IsValid() || Descriptor == nullptr || Descriptor->ClassName != Source.ClassName || Source.Name.empty())
			throw std::invalid_argument("Instance graph snapshot contains an invalid record");
		InstanceRecord Record = Source;
		for (auto &[Name, Value] : Record.Properties)
			ValidateAndNormalizeProperty(*Descriptor, Name, Value);
		for (const auto &[Name, Value] : Descriptor->DefaultProperties)
			Record.Properties.try_emplace(Name, Value);
		ValidateRecordSemantics(*Descriptor, Record.Properties);
		Record.Children.clear();
		if (!Replacement.emplace(Record.ID, std::move(Record)).second)
			throw std::invalid_argument("Instance graph snapshot contains duplicate identities");
	}

	std::vector<util::UUID> ReplacementRoots;
	for (auto &[ID, Record] : Replacement)
	{
		if (!Record.Parent.IsValid())
		{
			ReplacementRoots.push_back(ID);
			continue;
		}
		auto Parent = Replacement.find(Record.Parent);
		if (Parent == Replacement.end() || Parent->first == ID)
			throw std::invalid_argument("Instance graph snapshot contains an invalid parent");
		Parent->second.Children.push_back(ID);
	}
	const auto SortChildren = [&Replacement](std::vector<util::UUID> &Children)
	{
		std::ranges::sort(Children, [&Replacement](const util::UUID &Left, const util::UUID &Right)
						  { return Replacement.at(Left).SiblingOrder < Replacement.at(Right).SiblingOrder; });
		for (usize Index = 0; Index < Children.size(); ++Index)
			Replacement.at(Children[Index]).SiblingOrder = static_cast<uint32>(Index);
	};
	SortChildren(ReplacementRoots);
	for (auto &[ID, Record] : Replacement)
	{
		(void)ID;
		SortChildren(Record.Children);
	}

	std::unordered_set<util::UUID> Visited;
	std::vector<util::UUID> Pending = ReplacementRoots;
	while (!Pending.empty())
	{
		const util::UUID ID = Pending.back();
		Pending.pop_back();
		if (!Visited.emplace(ID).second)
			throw std::invalid_argument("Instance graph snapshot contains a hierarchy cycle");
		const auto &Children = Replacement.at(ID).Children;
		Pending.insert(Pending.end(), Children.begin(), Children.end());
	}
	if (Visited.size() != Replacement.size())
		throw std::invalid_argument("Instance graph snapshot contains unreachable instances");

	const auto FindService = [&Replacement](const InstanceClassID ClassID)
	{
		util::UUID Found;
		for (const auto &[ID, Record] : Replacement)
		{
			if (Record.ClassID != ClassID)
				continue;
			if (Found.IsValid() || Record.Parent.IsValid() || !Record.Protected)
				throw std::invalid_argument("Instance graph snapshot service invariant is invalid");
			Found = ID;
		}
		if (!Found.IsValid())
			throw std::invalid_argument("Instance graph snapshot is missing a required service");
		return Found;
	};
	const util::UUID NewWorkspace = FindService(class_ids::Workspace);
	const util::UUID NewLighting = FindService(class_ids::Lighting);
	const util::UUID NewGUI = FindService(class_ids::GUI);
	const util::UUID NewAudio = FindService(class_ids::Audio);
	const util::UUID NewScripts = FindService(class_ids::Scripts);

	std::unique_lock Lock(this->Mutex);
	this->Records = std::move(Replacement);
	this->Roots = std::move(ReplacementRoots);
	this->Workspace = NewWorkspace;
	this->Lighting = NewLighting;
	this->GUI = NewGUI;
	this->Audio = NewAudio;
	this->Scripts = NewScripts;
	this->Revision = Snapshot.Revision;
}

uint64 InstanceGraph::GetRevision() const noexcept
{
	std::shared_lock Lock(this->Mutex);
	return this->Revision;
}

const InstanceTypeRegistry &InstanceGraph::GetTypes() const noexcept
{
	return *this->Types;
}

util::UUID InstanceGraph::GetWorkspace() const noexcept
{
	return this->Workspace;
}
util::UUID InstanceGraph::GetLighting() const noexcept
{
	return this->Lighting;
}
util::UUID InstanceGraph::GetGUI() const noexcept
{
	return this->GUI;
}
util::UUID InstanceGraph::GetAudio() const noexcept
{
	return this->Audio;
}
util::UUID InstanceGraph::GetScripts() const noexcept
{
	return this->Scripts;
}

util::UUID InstanceGraph::FindByClass(const InstanceClassID &ClassID) const
{
	std::shared_lock Lock(this->Mutex);
	for (const auto &[ID, Record] : this->Records)
	{
		if (Record.ClassID == ClassID)
			return ID;
	}
	return {};
}

void InstanceGraph::AssertOwnerThread() const
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw std::logic_error("InstanceGraph mutation executed outside its owner thread");
}

void InstanceGraph::CreateServices()
{
	const auto Add = [this](const InstanceClassID ID, const string_view Name)
	{
		const std::shared_ptr<const InstanceTypeDescriptor> Descriptor = this->Types->Find(ID);
		if (Descriptor == nullptr || !Descriptor->Service)
			throw std::logic_error("Required service class is not registered");
		const util::UUID InstanceID = util::UUID::GenerateRandomUUID();
		this->Records.emplace(InstanceID, InstanceRecord{.ID = InstanceID,
														 .ClassID = ID,
														 .ClassName = Descriptor->ClassName,
														 .Name = string(Name),
														 .SiblingOrder = static_cast<uint32>(this->Roots.size()),
														 .Protected = true});
		this->Roots.push_back(InstanceID);
		return InstanceID;
	};
	this->Workspace = Add(class_ids::Workspace, "Workspace");
	this->Lighting = Add(class_ids::Lighting, "Lighting");
	this->GUI = Add(class_ids::GUI, "GUI");
	this->Audio = Add(class_ids::Audio, "Audio");
	this->Scripts = Add(class_ids::Scripts, "Scripts");
}

bool InstanceGraph::IsDescendantUnlocked(const util::UUID &Candidate, const util::UUID &Ancestor) const
{
	util::UUID Current = Candidate;
	while (Current.IsValid())
	{
		if (Current == Ancestor)
			return true;
		const auto Iterator = this->Records.find(Current);
		if (Iterator == this->Records.end())
			return false;
		Current = Iterator->second.Parent;
	}
	return false;
}

string InstanceGraph::MakeUniqueNameUnlocked(const util::UUID &Parent, string BaseName) const
{
	const auto &Siblings = Parent.IsValid() ? this->Records.at(Parent).Children : this->Roots;
	const auto IsTaken = [this, &Siblings](const string_view Candidate)
	{
		return std::ranges::any_of(Siblings,
								   [this, Candidate](const util::UUID &Sibling) { return this->Records.at(Sibling).Name == Candidate; });
	};
	if (!IsTaken(BaseName))
		return BaseName;
	for (uint32 Suffix = 2; Suffix != std::numeric_limits<uint32>::max(); ++Suffix)
	{
		string Candidate = BaseName + " " + std::to_string(Suffix);
		if (!IsTaken(Candidate))
			return Candidate;
	}
	throw std::overflow_error("Unable to generate a unique instance name");
}

void InstanceGraph::NormalizeSiblingOrderUnlocked(const util::UUID &Parent)
{
	auto &Siblings = Parent.IsValid() ? this->Records.at(Parent).Children : this->Roots;
	for (usize Index = 0; Index < Siblings.size(); ++Index)
		this->Records.at(Siblings[Index]).SiblingOrder = static_cast<uint32>(Index);
}

void InstanceGraph::Touch() noexcept
{
	this->Revision = AdvanceRevision(this->Revision);
}
} // namespace editor::instance
