#include "SceneDocument.h"

#include "Source/component/object/CObjectHierarchyComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/component/object/CObjectCameraComponent.h"
#include "Source/component/object/CObjectBehaviorComponent.h"
#include "Source/component/object/CObjectAnimationComponent.h"
#include "Source/component/object/CObjectLightComponents.h"
#include "Source/component/object/CObjectMeshComponent.h"
#include "Source/editor/asset/PrimitiveMeshFactory.h"
#include "Source/resource/asset/AssetManager.h"
#include "Source/runtime/behavior/BehaviorRegistry.h"

#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace editor::document
{
namespace
{
[[nodiscard]] uint64 AdvanceRevision(const uint64 Revision, uint64 Delta) noexcept
{
	if (Delta == 0)
		return Revision;
	const uint64 RemainingBeforeRollover = std::numeric_limits<uint64>::max() - Revision;
	if (Delta <= RemainingBeforeRollover)
		return Revision + Delta;
	Delta -= RemainingBeforeRollover + 1U;
	return 1U + Delta % std::numeric_limits<uint64>::max();
}

[[nodiscard]] uint64 RevisionDistance(const uint64 Baseline, const uint64 Current) noexcept
{
	if (Current >= Baseline)
		return Current - Baseline;
	return (std::numeric_limits<uint64>::max() - Baseline) + Current;
}

[[nodiscard]] asset::PrimitiveShape ResolvePrimitiveShape(const instance::InstanceRecord &Record)
{
	const auto ShapeProperty = Record.Properties.find("Shape");
	if (ShapeProperty == Record.Properties.end() || !std::holds_alternative<string>(ShapeProperty->second))
		return asset::PrimitiveShape::Box;
	const string &Name = std::get<string>(ShapeProperty->second);
	for (usize Index = 0; Index < static_cast<usize>(asset::PrimitiveShape::Count); ++Index)
	{
		const auto Candidate = static_cast<asset::PrimitiveShape>(Index);
		if (asset::PrimitiveMeshFactory::GetName(Candidate) == Name)
			return Candidate;
	}
	throw std::invalid_argument("Part Shape property is not a registered primitive shape");
}

template <typename ValueType>
[[nodiscard]] const ValueType &GetInstanceProperty(const instance::InstanceRecord &Record, const string_view Name)
{
	const auto Property = Record.Properties.find(string(Name));
	if (Property == Record.Properties.end() || !std::holds_alternative<ValueType>(Property->second))
		throw std::logic_error("Instance property schema mismatch: " + string(Name));
	return std::get<ValueType>(Property->second);
}

[[nodiscard]] components::ShadowResolution ResolveShadowResolution(const uint64 Resolution)
{
	switch (Resolution)
	{
	case 256:
	case 512:
	case 1'024:
	case 2'048:
	case 4'096:
	case 8'192:
		return static_cast<components::ShadowResolution>(Resolution);
	default:
		throw std::invalid_argument("ShadowResolution must be a supported power-of-two tier");
	}
}

void ApplyShadowProperties(const instance::InstanceRecord &Record, components::LightShadowSettings &Shadows)
{
	Shadows.CastShadows = GetInstanceProperty<bool>(Record, "CastShadows");
	Shadows.Resolution = ResolveShadowResolution(GetInstanceProperty<uint64>(Record, "ShadowResolution"));
	Shadows.ConstantBias = static_cast<float32>(GetInstanceProperty<float64>(Record, "ShadowConstantBias"));
	Shadows.SlopeBias = static_cast<float32>(GetInstanceProperty<float64>(Record, "ShadowSlopeBias"));
	Shadows.NormalBias = static_cast<float32>(GetInstanceProperty<float64>(Record, "ShadowNormalBias"));
	Shadows.FilterRadius = static_cast<float32>(GetInstanceProperty<float64>(Record, "ShadowFilterRadius"));
}
} // namespace

SceneDocument::SceneDocument(string Name, const world::SceneCapacitySpecification Capacity, const util::UUID ID,
							 const usize CommandHistoryCapacity)
	: ID(ID), Name(std::move(Name)), InstanceTypes(), Instances(InstanceTypes), Scene(std::make_unique<world::Scene>(Capacity)),
	  History(
		  CommandHistoryCapacity,
		  [this]()
		  {
			  this->Selection.Prune(this->Instances);
			  this->MarkModified();
		  },
		  [this]() noexcept { return this->GetRevision(); }),
	  OwnerThread(std::this_thread::get_id())
{
	if (this->Name.empty())
		throw std::invalid_argument("Scene document name cannot be empty");
	if (!this->ID.IsValid())
		throw std::invalid_argument("Scene document requires a valid persistent identity");
	this->RevisionSceneBaseline = this->Scene->GetMutationRevision();
}

world::ObjectHandle SceneDocument::CreateObject(string Name, const world::ObjectHandle Parent, const util::UUID PersistentID)
{
	return this->CreateObject(SceneObjectSpecification{.Name = std::move(Name), .Parent = Parent, .PersistentID = PersistentID});
}

world::ObjectHandle SceneDocument::CreateObject(SceneObjectSpecification Specification)
{
	this->AssertOwnerThread();
	if (Specification.Name.empty())
		throw std::invalid_argument("Scene object name cannot be empty");
	if (!Specification.PersistentID.IsValid())
		throw std::invalid_argument("Scene object requires a valid persistent identity");
	if (this->Scene->FindObject(Specification.PersistentID).IsValid())
		throw std::invalid_argument("Scene object persistent identity is already present");
	if (Specification.Parent.IsValid() && !this->Scene->Contains(Specification.Parent))
		throw world::InvalidObjectHandleException(Specification.Parent);
	const string InstanceName = Specification.Name;

	const world::ObjectHandle Object = this->Scene->CreateObject();
	try
	{
		(void)this->Scene->AddComponent<components::CObjectIdentityComponent>(Object, std::move(Specification.Name),
																			  Specification.PersistentID);
		(void)this->Scene->AddComponent<components::CObjectTransformComponent>(Object);
		(void)this->Scene->AddComponent<components::CObjectHierarchyComponent>(Object);
		if (Specification.Parent.IsValid())
			this->Scene->SetParent(Object, Specification.Parent);

		util::UUID InstanceParent = this->Instances.GetWorkspace();
		if (Specification.Parent.IsValid())
		{
			const auto ParentIdentity = this->Scene->GetComponent<components::CObjectIdentityComponent>(Specification.Parent);
			if (ParentIdentity.IsValid())
			{
				const auto Access = this->Scene->Read();
				InstanceParent = Access.Resolve(ParentIdentity).GetPersistentID();
			}
		}
		if (!this->Instances.Contains(Specification.PersistentID))
			(void)this->Instances.Create(instance::class_ids::Model, InstanceParent, InstanceName, Specification.PersistentID);
	}
	catch (...)
	{
		if (this->Instances.Contains(Specification.PersistentID))
			this->Instances.Destroy(Specification.PersistentID);
		this->Scene->DestroyObject(Object);
		throw;
	}

	const auto Identity = this->Scene->GetComponent<components::CObjectIdentityComponent>(Object);
	{
		auto Access = this->Scene->Read();
		this->Selection.SelectOnly(Access.Resolve(Identity).GetPersistentID());
	}
	this->MarkModified();
	return Object;
}

util::UUID SceneDocument::CreateInstance(const instance::InstanceClassID ClassID, const util::UUID Parent, string Name, const util::UUID ID,
										 instance::InstancePropertyMap InitialProperties)
{
	this->AssertOwnerThread();
	(void)this->Instances.Create(ClassID, Parent, std::move(Name), ID, std::move(InitialProperties));
	try
	{
		this->CreateRuntimeBacking(ID);
		if (ClassID == instance::class_ids::Script && Parent.IsValid())
			this->SynchronizeRuntimeBehaviors(Parent);
		const util::UUID AnimationModel = this->FindAnimationModel(ID);
		if (AnimationModel.IsValid())
			this->SynchronizeRuntimeAnimations(AnimationModel);
	}
	catch (...)
	{
		this->Instances.Destroy(ID);
		throw;
	}
	this->Selection.SelectOnly(ID);
	this->MarkModified();
	return ID;
}

void SceneDocument::DestroyInstance(const util::UUID &ID)
{
	this->AssertOwnerThread();
	const instance::InstanceRecord RootRecord = this->Instances.Get(ID);
	const util::UUID AnimationModel = this->FindAnimationModel(ID);
	const instance::InstanceGraphSnapshot Snapshot = this->Instances.Snapshot();
	std::unordered_set<util::UUID> Subtree{ID};
	for (const instance::InstanceRecord &Record : Snapshot.Instances)
	{
		if (Record.ID == ID || (Record.Parent.IsValid() && Subtree.contains(Record.Parent)))
			Subtree.emplace(Record.ID);
	}
	for (const util::UUID &SubtreeID : Subtree)
	{
		const world::ObjectHandle RuntimeObject = this->Scene->FindObject(SubtreeID);
		if (RuntimeObject.IsValid())
			this->Scene->DestroyObject(RuntimeObject);
	}
	this->Instances.Destroy(ID);
	if (RootRecord.ClassID == instance::class_ids::Script && RootRecord.Parent.IsValid())
		this->SynchronizeRuntimeBehaviors(RootRecord.Parent);
	if (AnimationModel.IsValid() && this->Instances.Contains(AnimationModel))
		this->SynchronizeRuntimeAnimations(AnimationModel);
	this->Selection.Prune(this->Instances);
	this->MarkModified();
}

void SceneDocument::RenameInstance(const util::UUID &ID, string Name)
{
	this->AssertOwnerThread();
	this->Instances.Rename(ID, Name);
	const world::ObjectHandle RuntimeObject = this->Scene->FindObject(ID);
	if (RuntimeObject.IsValid())
	{
		const auto Identity = this->Scene->GetComponent<components::CObjectIdentityComponent>(RuntimeObject);
		if (Identity.IsValid())
		{
			auto Access = this->Scene->Write();
			Access.Resolve(Identity).SetName(this->Instances.Get(ID).Name);
		}
	}
	this->MarkModified();
}

void SceneDocument::ReparentInstance(const util::UUID &ID, const util::UUID &Parent, const uint32 SiblingOrder)
{
	this->AssertOwnerThread();
	const instance::InstanceRecord Before = this->Instances.Get(ID);
	const util::UUID BeforeAnimationModel = this->FindAnimationModel(ID);
	this->Instances.Reparent(ID, Parent, SiblingOrder);
	this->SynchronizeRuntimeSubtree(ID);
	if (Before.ClassID == instance::class_ids::Script)
	{
		if (Before.Parent.IsValid())
			this->SynchronizeRuntimeBehaviors(Before.Parent);
		if (Parent.IsValid())
			this->SynchronizeRuntimeBehaviors(Parent);
	}
	const util::UUID AfterAnimationModel = this->FindAnimationModel(ID);
	if (BeforeAnimationModel.IsValid() && this->Instances.Contains(BeforeAnimationModel))
		this->SynchronizeRuntimeAnimations(BeforeAnimationModel);
	if (AfterAnimationModel.IsValid() && AfterAnimationModel != BeforeAnimationModel)
		this->SynchronizeRuntimeAnimations(AfterAnimationModel);
	this->MarkModified();
}

void SceneDocument::SetInstanceProperty(const util::UUID &ID, string Name, instance::InstancePropertyValue Value)
{
	this->AssertOwnerThread();
	const instance::InstanceRecord Before = this->Instances.Get(ID);
	const util::UUID AnimationModel = this->FindAnimationModel(ID);
	if (Before.ClassID == instance::class_ids::Model && (Name == "PivotPosition" || Name == "PivotRotation" || Name == "PivotScale"))
	{
		glm::vec3 Position = GetInstanceProperty<glm::vec3>(Before, "PivotPosition");
		glm::quat Rotation = GetInstanceProperty<glm::quat>(Before, "PivotRotation");
		glm::vec3 Scale = GetInstanceProperty<glm::vec3>(Before, "PivotScale");
		if (Name == "PivotPosition")
			Position = std::get<glm::vec3>(Value);
		else if (Name == "PivotRotation")
			Rotation = std::get<glm::quat>(Value);
		else
			Scale = std::get<glm::vec3>(Value);
		this->SetInstanceWorldTransform(ID, Position, Rotation, Scale);
		return;
	}
	this->Instances.SetProperty(ID, std::move(Name), std::move(Value));
	this->SynchronizeRuntimeSubtree(ID);
	if (Before.ClassID == instance::class_ids::Script && Before.Parent.IsValid())
		this->SynchronizeRuntimeBehaviors(Before.Parent);
	if (AnimationModel.IsValid() &&
		(Before.ClassID == instance::class_ids::Animator || Before.ClassID == instance::class_ids::AnimationTrack))
		this->SynchronizeRuntimeAnimations(AnimationModel);
	this->MarkModified();
}

void SceneDocument::RemoveInstanceProperty(const util::UUID &ID, const string_view Name)
{
	this->AssertOwnerThread();
	const instance::InstanceRecord Before = this->Instances.Get(ID);
	this->Instances.RemoveProperty(ID, Name);
	if (Before.ClassID == instance::class_ids::Script && Before.Parent.IsValid())
		this->SynchronizeRuntimeBehaviors(Before.Parent);
	this->MarkModified();
}

void SceneDocument::SetInstanceWorldTransform(const util::UUID &ID, const glm::vec3 &Position, const glm::quat &Rotation,
											  const glm::vec3 &Scale)
{
	this->AssertOwnerThread();
	this->Instances.SetWorldTransform(ID, Position, Rotation, Scale);
	this->SynchronizeRuntimeSubtree(ID);
	this->MarkModified();
}

void SceneDocument::ConfigureRuntimeAssets(resource::AssetManager &Assets, asset::PrimitiveMeshFactory &Primitives,
										   runtime::behavior::BehaviorRegistry *Behaviors)
{
	this->AssertOwnerThread();
	this->Assets = &Assets;
	this->Primitives = &Primitives;
	this->Behaviors = Behaviors;
	this->SynchronizeAllRuntimeBackings();
}

void SceneDocument::SynchronizeAllRuntimeBackings()
{
	this->AssertOwnerThread();
	for (const instance::InstanceRecord &Record : this->Instances.Snapshot().Instances)
	{
		if (!this->Scene->FindObject(Record.ID).IsValid())
			this->CreateRuntimeBacking(Record.ID);
		else
			this->SynchronizeRuntimeBacking(Record.ID);
	}
	for (const instance::InstanceRecord &Record : this->Instances.Snapshot().Instances)
	{
		if (this->Scene->FindObject(Record.ID).IsValid())
			this->SynchronizeRuntimeBehaviors(Record.ID);
		if (Record.ClassID == instance::class_ids::Model)
			this->SynchronizeRuntimeAnimations(Record.ID);
	}
}

void SceneDocument::DestroyObject(const util::UUID &PersistentID)
{
	this->AssertOwnerThread();
	const world::ObjectHandle Object = this->Scene->FindObject(PersistentID);
	if (!Object.IsValid())
		throw std::out_of_range("Cannot destroy an object identity that is not present in the scene document");
	this->Selection.Remove(PersistentID);
	this->Scene->DestroyObject(Object);
	if (this->Instances.Contains(PersistentID))
		this->Instances.Destroy(PersistentID);
	this->Selection.Prune(*this->Scene);
	this->MarkModified();
}

void SceneDocument::SetParent(const util::UUID &Object, const util::UUID &Parent, const uint32 SiblingOrder)
{
	this->AssertOwnerThread();
	const world::ObjectHandle ObjectHandle = this->Scene->FindObject(Object);
	if (!ObjectHandle.IsValid())
		throw std::out_of_range("Cannot parent an object identity that is not present in the scene document");
	const world::ObjectHandle ParentHandle = Parent.IsValid() ? this->Scene->FindObject(Parent) : world::ObjectHandle{};
	if (Parent.IsValid() && !ParentHandle.IsValid())
		throw std::out_of_range("Cannot use a parent identity that is not present in the scene document");
	this->Scene->SetParent(ObjectHandle, ParentHandle, SiblingOrder);
	if (this->Instances.Contains(Object))
	{
		const util::UUID InstanceParent = Parent.IsValid() ? Parent : this->Instances.GetWorkspace();
		this->Instances.Reparent(Object, InstanceParent, SiblingOrder);
	}
	this->MarkModified();
}

void SceneDocument::Execute(commands::EditorCommandPtr Command)
{
	this->AssertOwnerThread();
	this->History.Execute(std::move(Command));
}

void SceneDocument::Undo()
{
	this->AssertOwnerThread();
	if (!this->History.CanUndo())
		return;
	this->History.Undo();
}

void SceneDocument::Redo()
{
	this->AssertOwnerThread();
	if (!this->History.CanRedo())
		return;
	this->History.Redo();
}

void SceneDocument::MarkSaved(std::filesystem::path Path)
{
	this->AssertOwnerThread();
	if (Path.empty())
		throw std::invalid_argument("Saved scene path cannot be empty");
	this->Path = std::move(Path);
	this->SavedRevision = this->GetRevision();
}

void SceneDocument::MarkRecovered(std::filesystem::path OriginalPath)
{
	this->AssertOwnerThread();
	this->Path = std::move(OriginalPath);
	const uint64 CurrentRevision = this->GetRevision();
	this->SavedRevision = CurrentRevision == 1U ? 0U : CurrentRevision - 1U;
}

void SceneDocument::MarkModified() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	const uint64 SceneRevision = this->Scene->GetMutationRevision();
	const uint64 SceneDelta = RevisionDistance(this->RevisionSceneBaseline, SceneRevision);
	this->Revision = AdvanceRevision(this->Revision, SceneDelta == 0 ? 1U : SceneDelta);
	this->RevisionSceneBaseline = SceneRevision;
}

world::Scene &SceneDocument::GetScene() noexcept
{
	return *this->Scene;
}

const world::Scene &SceneDocument::GetScene() const noexcept
{
	return *this->Scene;
}

instance::InstanceGraph &SceneDocument::GetInstances() noexcept
{
	return this->Instances;
}

const instance::InstanceGraph &SceneDocument::GetInstances() const noexcept
{
	return this->Instances;
}

instance::InstanceTypeRegistry &SceneDocument::GetInstanceTypes() noexcept
{
	return this->InstanceTypes;
}

const instance::InstanceTypeRegistry &SceneDocument::GetInstanceTypes() const noexcept
{
	return this->InstanceTypes;
}

SelectionSet &SceneDocument::GetSelection() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	return this->Selection;
}

const SelectionSet &SceneDocument::GetSelection() const noexcept
{
	return this->Selection;
}

commands::CommandHistory &SceneDocument::GetHistory() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	return this->History;
}

const util::UUID &SceneDocument::GetID() const noexcept
{
	return this->ID;
}

const string &SceneDocument::GetName() const noexcept
{
	return this->Name;
}

void SceneDocument::SetName(string Name)
{
	this->AssertOwnerThread();
	if (Name.empty())
		throw std::invalid_argument("Scene document name cannot be empty");
	this->Name = std::move(Name);
	this->MarkModified();
}

const std::filesystem::path &SceneDocument::GetPath() const noexcept
{
	return this->Path;
}

uint64 SceneDocument::GetRevision() const noexcept
{
	const uint64 SceneRevision = this->Scene->GetMutationRevision();
	return AdvanceRevision(this->Revision, RevisionDistance(this->RevisionSceneBaseline, SceneRevision));
}

bool SceneDocument::IsDirty() const noexcept
{
	return this->GetRevision() != this->SavedRevision;
}

const string &SceneDocument::GetPreservedSerializationData() const noexcept
{
	return this->PreservedSerializationData;
}

void SceneDocument::SetPreservedSerializationData(string Data)
{
	this->AssertOwnerThread();
	this->PreservedSerializationData = std::move(Data);
}

world::ObjectHandle SceneDocument::CreateRuntimeObject(string Name, const util::UUID &PersistentID)
{
	if (this->Scene->FindObject(PersistentID).IsValid())
		throw std::invalid_argument("Runtime object identity already exists");
	const world::ObjectHandle Object = this->Scene->CreateObject();
	try
	{
		(void)this->Scene->AddComponent<components::CObjectIdentityComponent>(Object, std::move(Name), PersistentID);
		(void)this->Scene->AddComponent<components::CObjectTransformComponent>(Object);
		(void)this->Scene->AddComponent<components::CObjectHierarchyComponent>(Object);
	}
	catch (...)
	{
		this->Scene->DestroyObject(Object);
		throw;
	}
	return Object;
}

void SceneDocument::CreateRuntimeBacking(const util::UUID &ID)
{
	const instance::InstanceRecord Record = this->Instances.Get(ID);
	if (this->Instances.GetActivation(ID).State != instance::InstanceActivationState::Active)
		return;
	const bool RequiresObject = Record.ClassID == instance::class_ids::Model || Record.ClassID == instance::class_ids::Part ||
								Record.ClassID == instance::class_ids::MeshPart || Record.ClassID == instance::class_ids::Camera ||
								Record.ClassID == instance::class_ids::DirectionalLight ||
								Record.ClassID == instance::class_ids::PointLight || Record.ClassID == instance::class_ids::SpotLight;
	if (!RequiresObject)
		return;
	const world::ObjectHandle Object = this->CreateRuntimeObject(Record.Name, ID);
	try
	{
		if (Record.ClassID == instance::class_ids::Camera)
			(void)this->Scene->AddComponent<components::CObjectCameraComponent>(Object);
		else if (Record.ClassID == instance::class_ids::DirectionalLight)
			(void)this->Scene->AddComponent<components::CObjectDirectionalLightComponent>(Object);
		else if (Record.ClassID == instance::class_ids::PointLight)
			(void)this->Scene->AddComponent<components::CObjectPointLightComponent>(Object);
		else if (Record.ClassID == instance::class_ids::SpotLight)
			(void)this->Scene->AddComponent<components::CObjectSpotLightComponent>(Object);
		else if (Record.ClassID == instance::class_ids::Part && this->Primitives != nullptr)
			(void)this->Scene->AddComponent<components::CObjectMeshComponent>(Object,
																			  this->Primitives->GetModel(ResolvePrimitiveShape(Record)));
		else if (Record.ClassID == instance::class_ids::MeshPart && this->Assets != nullptr)
		{
			const auto ModelProperty = Record.Properties.find("Model");
			if (ModelProperty != Record.Properties.end() && std::holds_alternative<instance::InstanceAssetReference>(ModelProperty->second))
			{
				const instance::InstanceAssetReference &Reference = std::get<instance::InstanceAssetReference>(ModelProperty->second);
				if (!Reference.ID.empty() && Reference.Type == resource::AssetType::Model)
					(void)this->Scene->AddComponent<components::CObjectMeshComponent>(
						Object, this->Assets->GetAssetByID<resource::ModelAsset>(Reference.ID));
			}
		}
		this->SynchronizeRuntimeBacking(ID);
	}
	catch (...)
	{
		this->Scene->DestroyObject(Object);
		throw;
	}
}

void SceneDocument::SynchronizeRuntimeBacking(const util::UUID &ID)
{
	world::ObjectHandle Object = this->Scene->FindObject(ID);
	if (this->Instances.GetActivation(ID).State != instance::InstanceActivationState::Active)
	{
		if (Object.IsValid())
			this->Scene->DestroyObject(Object);
		return;
	}
	if (!Object.IsValid())
	{
		this->CreateRuntimeBacking(ID);
		return;
	}
	const instance::InstanceRecord Record = this->Instances.Get(ID);
	const auto ResolveTransform = [this](const auto &Self, const instance::InstanceRecord &Current, glm::vec3 &Position,
										 glm::quat &Rotation, glm::vec3 &Scale) -> void
	{
		const auto PositionValue = Current.Properties.find("Position");
		const auto RotationValue = Current.Properties.find("Rotation");
		const auto ScaleValue = Current.Properties.find("Scale");
		if (PositionValue != Current.Properties.end() && std::holds_alternative<glm::vec3>(PositionValue->second))
			Position = std::get<glm::vec3>(PositionValue->second);
		if (RotationValue != Current.Properties.end() && std::holds_alternative<glm::quat>(RotationValue->second))
			Rotation = std::get<glm::quat>(RotationValue->second);
		if (ScaleValue != Current.Properties.end() && std::holds_alternative<glm::vec3>(ScaleValue->second))
			Scale = std::get<glm::vec3>(ScaleValue->second);
		if (Current.ClassID == instance::class_ids::Attachment && Current.Parent.IsValid())
		{
			const glm::vec3 LocalPosition = Position;
			const glm::quat LocalRotation = Rotation;
			const instance::InstanceRecord Parent = this->Instances.Get(Current.Parent);
			glm::vec3 ParentPosition{0.0F};
			glm::quat ParentRotation{1.0F, 0.0F, 0.0F, 0.0F};
			glm::vec3 ParentScale{1.0F};
			Self(Self, Parent, ParentPosition, ParentRotation, ParentScale);
			Position = ParentPosition + ParentRotation * (ParentScale * LocalPosition);
			Rotation = glm::normalize(ParentRotation * LocalRotation);
			Scale = ParentScale;
		}
	};
	glm::vec3 Position{0.0F};
	glm::quat Rotation{1.0F, 0.0F, 0.0F, 0.0F};
	glm::vec3 Scale{1.0F};
	if (Record.ClassID == instance::class_ids::Model)
	{
		Position = std::get<glm::vec3>(Record.Properties.at("PivotPosition"));
		Rotation = std::get<glm::quat>(Record.Properties.at("PivotRotation"));
		Scale = std::get<glm::vec3>(Record.Properties.at("PivotScale"));
	}
	else
		ResolveTransform(ResolveTransform, Record, Position, Rotation, Scale);
	const auto Transform = this->Scene->GetComponent<components::CObjectTransformComponent>(Object);
	if (Transform.IsValid())
	{
		auto Access = this->Scene->Write();
		Access.Resolve(Transform).SetTransform(Position, Rotation, Scale);
	}
	if (Record.ClassID == instance::class_ids::Camera)
	{
		const auto Camera = this->Scene->GetComponent<components::CObjectCameraComponent>(Object);
		if (Camera.IsValid())
		{
			auto Access = this->Scene->Write();
			components::CObjectCameraComponent &Component = Access.Resolve(Camera);
			const string &Projection = GetInstanceProperty<string>(Record, "Projection");
			if (Projection == "Perspective")
				Component.SetProjection(components::CameraProjection::Perspective);
			else if (Projection == "Orthographic")
				Component.SetProjection(components::CameraProjection::Orthographic);
			else
				throw std::invalid_argument("Camera Projection must be Perspective or Orthographic");
			Component.SetVerticalFieldOfViewDegrees(static_cast<float32>(GetInstanceProperty<float64>(Record, "FieldOfView")));
			Component.SetOrthographicHeight(static_cast<float32>(GetInstanceProperty<float64>(Record, "OrthographicHeight")));
			Component.SetClipPlanes(static_cast<float32>(GetInstanceProperty<float64>(Record, "NearPlane")),
									static_cast<float32>(GetInstanceProperty<float64>(Record, "FarPlane")));
			Component.SetExposureCompensation(static_cast<float32>(GetInstanceProperty<float64>(Record, "ExposureCompensation")));
			Component.SetPrimary(GetInstanceProperty<bool>(Record, "Primary"));
			Component.SetTemporalJitterEnabled(GetInstanceProperty<bool>(Record, "TemporalJitter"));
		}
	}
	else if (Record.ClassID == instance::class_ids::DirectionalLight)
	{
		const auto Light = this->Scene->GetComponent<components::CObjectDirectionalLightComponent>(Object);
		if (Light.IsValid())
		{
			auto Access = this->Scene->Write();
			components::CObjectDirectionalLightComponent &Component = Access.Resolve(Light);
			Component.SetColor(GetInstanceProperty<glm::vec3>(Record, "Color"));
			Component.SetIlluminanceLux(static_cast<float32>(GetInstanceProperty<float64>(Record, "IlluminanceLux")));
			Component.SetAngularDiameterDegrees(static_cast<float32>(GetInstanceProperty<float64>(Record, "AngularDiameterDegrees")));
			Component.SetCascadeCount(static_cast<uint32>(GetInstanceProperty<uint64>(Record, "CascadeCount")));
			Component.SetCascadeDistributionExponent(
				static_cast<float32>(GetInstanceProperty<float64>(Record, "CascadeDistributionExponent")));
			ApplyShadowProperties(Record, Component.GetShadowSettings());
		}
	}
	else if (Record.ClassID == instance::class_ids::PointLight)
	{
		const auto Light = this->Scene->GetComponent<components::CObjectPointLightComponent>(Object);
		if (Light.IsValid())
		{
			auto Access = this->Scene->Write();
			components::CObjectPointLightComponent &Component = Access.Resolve(Light);
			Component.SetColor(GetInstanceProperty<glm::vec3>(Record, "Color"));
			Component.SetLuminousPowerLumens(static_cast<float32>(GetInstanceProperty<float64>(Record, "LuminousPowerLumens")));
			Component.SetRange(static_cast<float32>(GetInstanceProperty<float64>(Record, "Range")));
			Component.SetSourceRadius(static_cast<float32>(GetInstanceProperty<float64>(Record, "SourceRadius")));
			ApplyShadowProperties(Record, Component.GetShadowSettings());
		}
	}
	else if (Record.ClassID == instance::class_ids::SpotLight)
	{
		const auto Light = this->Scene->GetComponent<components::CObjectSpotLightComponent>(Object);
		if (Light.IsValid())
		{
			auto Access = this->Scene->Write();
			components::CObjectSpotLightComponent &Component = Access.Resolve(Light);
			Component.SetColor(GetInstanceProperty<glm::vec3>(Record, "Color"));
			Component.SetLuminousPowerLumens(static_cast<float32>(GetInstanceProperty<float64>(Record, "LuminousPowerLumens")));
			Component.SetRange(static_cast<float32>(GetInstanceProperty<float64>(Record, "Range")));
			Component.SetConeAngles(static_cast<float32>(GetInstanceProperty<float64>(Record, "InnerConeDegrees")),
									static_cast<float32>(GetInstanceProperty<float64>(Record, "OuterConeDegrees")));
			ApplyShadowProperties(Record, Component.GetShadowSettings());
		}
	}
	if (Record.ClassID == instance::class_ids::Part && this->Primitives != nullptr)
	{
		const resource::AssetHandle<resource::ModelAsset> Model = this->Primitives->GetModel(ResolvePrimitiveShape(Record));
		const auto Mesh = this->Scene->GetComponent<components::CObjectMeshComponent>(Object);
		if (Mesh.IsValid())
		{
			auto Access = this->Scene->Write();
			Access.Resolve(Mesh).SetModel(Model);
		}
		else
			(void)this->Scene->AddComponent<components::CObjectMeshComponent>(Object, Model);
	}
	if (Record.ClassID == instance::class_ids::MeshPart && this->Assets != nullptr)
	{
		const auto ModelProperty = Record.Properties.find("Model");
		if (ModelProperty != Record.Properties.end() && std::holds_alternative<instance::InstanceAssetReference>(ModelProperty->second))
		{
			const instance::InstanceAssetReference &Reference = std::get<instance::InstanceAssetReference>(ModelProperty->second);
			if (!Reference.ID.empty() && Reference.Type == resource::AssetType::Model)
			{
				const resource::AssetHandle<resource::ModelAsset> Model = this->Assets->GetAssetByID<resource::ModelAsset>(Reference.ID);
				const auto Mesh = this->Scene->GetComponent<components::CObjectMeshComponent>(Object);
				if (Mesh.IsValid())
				{
					auto Access = this->Scene->Write();
					Access.Resolve(Mesh).SetModel(Model);
				}
				else
					(void)this->Scene->AddComponent<components::CObjectMeshComponent>(Object, Model);
			}
		}
	}
}

void SceneDocument::SynchronizeRuntimeSubtree(const util::UUID &ID)
{
	const instance::InstanceGraphSnapshot Snapshot = this->Instances.Snapshot();
	std::unordered_set<util::UUID> Subtree{ID};
	for (const instance::InstanceRecord &Record : Snapshot.Instances)
	{
		if (Record.ID == ID || (Record.Parent.IsValid() && Subtree.contains(Record.Parent)))
		{
			Subtree.emplace(Record.ID);
			this->SynchronizeRuntimeBacking(Record.ID);
			if (Record.ClassID == instance::class_ids::Script && Record.Parent.IsValid())
				this->SynchronizeRuntimeBehaviors(Record.Parent);
		}
	}
}

void SceneDocument::SynchronizeRuntimeBehaviors(const util::UUID &ParentID)
{
	const world::ObjectHandle ParentObject = this->Scene->FindObject(ParentID);
	if (!ParentObject.IsValid())
		return;
	std::vector<components::BehaviorInstance> Instances;
	if (this->Behaviors != nullptr && this->Instances.Contains(ParentID))
	{
		const instance::InstanceRecord Parent = this->Instances.Get(ParentID);
		Instances.reserve(Parent.Children.size());
		for (const util::UUID &ChildID : Parent.Children)
		{
			const instance::InstanceRecord Child = this->Instances.Get(ChildID);
			if (Child.ClassID != instance::class_ids::Script ||
				this->Instances.GetActivation(ChildID).State != instance::InstanceActivationState::Active)
			{
				continue;
			}
			const uint64 Type = GetInstanceProperty<uint64>(Child, "BehaviorType");
			const std::optional<runtime::behavior::BehaviorDescriptor> Descriptor = this->Behaviors->Find(Type);
			if (!Descriptor.has_value() || Descriptor->StableTypeID != GetInstanceProperty<util::UUID>(Child, "StableTypeID") ||
				Descriptor->Name != GetInstanceProperty<string>(Child, "BehaviorName") ||
				Descriptor->ModuleName != GetInstanceProperty<string>(Child, "ModuleName"))
			{
				continue;
			}
			components::BehaviorInstance Behavior{.InstanceID = Child.ID,
												  .Type = Descriptor->Type,
												  .TypeName = Descriptor->Name,
												  .ModuleName = Descriptor->ModuleName,
												  .StableTypeID = Descriptor->StableTypeID,
												  .SchemaVersion = static_cast<uint32>(GetInstanceProperty<uint64>(Child, "SchemaVersion")),
												  .Enabled = Child.Enabled};
			for (const auto &[Name, Value] : Child.Properties)
			{
				if (!Name.starts_with("Behavior."))
					continue;
				const string PropertyName = Name.substr(9);
				std::visit(
					[&Behavior, &PropertyName](const auto &TypedValue)
					{
						using ValueType = std::decay_t<decltype(TypedValue)>;
						if constexpr (!std::same_as<ValueType, instance::InstanceAssetReference>)
							Behavior.Properties.emplace(PropertyName, TypedValue);
					},
					Value);
			}
			runtime::behavior::BehaviorRegistry::NormalizeProperties(*Descriptor, Behavior.Properties);
			Instances.push_back(std::move(Behavior));
		}
	}

	const auto Component = this->Scene->GetComponent<components::CObjectBehaviorComponent>(ParentObject);
	if (Instances.empty())
	{
		if (Component.IsValid())
			this->Scene->RemoveComponent<components::CObjectBehaviorComponent>(ParentObject);
		return;
	}
	if (!Component.IsValid())
		(void)this->Scene->AddComponent<components::CObjectBehaviorComponent>(ParentObject);
	const auto Updated = this->Scene->GetComponent<components::CObjectBehaviorComponent>(ParentObject);
	auto Access = this->Scene->Write();
	Access.Resolve(Updated).ReplaceBehaviors(std::move(Instances));
}

util::UUID SceneDocument::FindAnimationModel(const util::UUID &ID) const
{
	if (!ID.IsValid() || !this->Instances.Contains(ID))
		return {};
	instance::InstanceRecord Current = this->Instances.Get(ID);
	while (true)
	{
		if (Current.ClassID == instance::class_ids::Model)
			return Current.ID;
		if (!Current.Parent.IsValid())
			return {};
		Current = this->Instances.Get(Current.Parent);
	}
}

void SceneDocument::SynchronizeRuntimeAnimations(const util::UUID &ModelID)
{
	if (this->Assets == nullptr || !ModelID.IsValid() || !this->Instances.Contains(ModelID))
		return;
	const instance::InstanceRecord Model = this->Instances.Get(ModelID);
	if (Model.ClassID != instance::class_ids::Model)
		throw std::invalid_argument("Animation synchronization requires a Model instance");

	std::vector<components::DirectAnimationTrack> Tracks;
	for (const util::UUID &AnimatorID : Model.Children)
	{
		const instance::InstanceRecord Animator = this->Instances.Get(AnimatorID);
		if (Animator.ClassID != instance::class_ids::Animator ||
			this->Instances.GetActivation(AnimatorID).State != instance::InstanceActivationState::Active)
		{
			continue;
		}
		for (const util::UUID &TrackID : Animator.Children)
		{
			const instance::InstanceRecord Track = this->Instances.Get(TrackID);
			if (Track.ClassID != instance::class_ids::AnimationTrack ||
				this->Instances.GetActivation(TrackID).State != instance::InstanceActivationState::Active)
			{
				continue;
			}
			const instance::InstanceAssetReference &Clip = GetInstanceProperty<instance::InstanceAssetReference>(Track, "Clip");
			if (Clip.Type != resource::AssetType::AnimationClip || Clip.ID.empty())
				continue;
			Tracks.push_back({.InstanceID = Track.ID,
							  .Clip = this->Assets->GetAssetByID<resource::AnimationClipAsset>(Clip.ID),
							  .Speed = static_cast<float32>(GetInstanceProperty<float64>(Track, "Speed")),
							  .Weight = static_cast<float32>(GetInstanceProperty<float64>(Track, "Weight")),
							  .Playing = GetInstanceProperty<bool>(Track, "Playing")});
		}
	}

	std::vector<util::UUID> Pending = Model.Children;
	while (!Pending.empty())
	{
		const util::UUID CandidateID = Pending.back();
		Pending.pop_back();
		const instance::InstanceRecord Candidate = this->Instances.Get(CandidateID);
		Pending.insert(Pending.end(), Candidate.Children.begin(), Candidate.Children.end());
		if (Candidate.ClassID != instance::class_ids::MeshPart && Candidate.ClassID != instance::class_ids::Part)
			continue;
		const world::ObjectHandle Object = this->Scene->FindObject(CandidateID);
		if (!Object.IsValid() || !this->Scene->GetComponent<components::CObjectMeshComponent>(Object).IsValid())
			continue;
		const auto Existing = this->Scene->GetComponent<components::CObjectAnimationComponent>(Object);
		if (Tracks.empty())
		{
			if (Existing.IsValid())
			{
				bool UsesDirectTracks = false;
				{
					const auto Access = this->Scene->Read();
					UsesDirectTracks = Access.Resolve(Existing).UsesDirectTracks();
				}
				if (UsesDirectTracks)
					this->Scene->RemoveComponent<components::CObjectAnimationComponent>(Object);
			}
			continue;
		}

		std::vector<components::DirectAnimationTrack> TargetTracks = Tracks;
		if (Existing.IsValid())
		{
			const auto Access = this->Scene->Read();
			const components::CObjectAnimationComponent &Component = Access.Resolve(Existing);
			for (components::DirectAnimationTrack &Target : TargetTracks)
			{
				const auto Previous =
					std::ranges::find(Component.GetDirectTracks(), Target.InstanceID, &components::DirectAnimationTrack::InstanceID);
				if (Previous != Component.GetDirectTracks().end())
				{
					Target.PreviousPlaybackTime = Previous->PreviousPlaybackTime;
					Target.PlaybackTime = Previous->PlaybackTime;
				}
			}
		}
		if (!Existing.IsValid())
			(void)this->Scene->AddComponent<components::CObjectAnimationComponent>(Object, std::move(TargetTracks));
		else
		{
			auto Access = this->Scene->Write();
			Access.Resolve(Existing).SetDirectTracks(std::move(TargetTracks));
		}
	}
}

void SceneDocument::AssertOwnerThread() const
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw std::logic_error("SceneDocument mutation must run on its owner thread");
}
} // namespace editor::document
