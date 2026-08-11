#include "EditorCoreValidation.h"

#include "src/editor/EditorSession.h"
#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectBehaviorComponent.h"
#include "src/component/object/CObjectCameraComponent.h"
#include "src/component/object/CObjectHierarchyComponent.h"
#include "src/component/object/CObjectLightComponents.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/editor/commands/PropertyEditCommand.h"
#include "src/editor/commands/MeshMaterialOverrideCommand.h"
#include "src/editor/commands/BehaviorCommands.h"
#include "src/editor/commands/CreatePrimitiveCommand.h"
#include "src/editor/commands/SceneObjectCommands.h"
#include "src/editor/commands/TransformEditCommand.h"
#include "src/editor/cook/CookPackageService.h"
#include "src/editor/asset/AssetContentService.h"
#include "src/editor/asset/AssetRegistry.h"
#include "src/editor/asset/AssetReloadService.h"
#include "src/editor/asset/AssetThumbnailService.h"
#include "src/editor/action/EditorActionRegistry.h"
#include "src/editor/document/SceneDocument.h"
#include "src/editor/hierarchy/SceneHierarchy.h"
#include "src/editor/material/MaterialDocument.h"
#include "src/editor/material/PrivateMaterialAssignmentService.h"
#include "src/editor/play/PlaySession.h"
#include "src/editor/reflection/ComponentReflection.h"
#include "src/core/io/CompressedArchive.h"
#include "src/core/io/SecurePath.h"
#include "src/editor/serialization/ProjectDescriptorSerializer.h"
#include "src/editor/serialization/SceneDocumentSerializer.h"
#include "src/editor/preferences/EditorContentBrowserStore.h"
#include "src/editor/ui/EditorLayoutStore.h"
#include "src/editor/ui/EditorDockspace.h"
#include "src/runtime/project/ProjectPackage.h"
#include "src/editor/viewport/EditorCameraController.h"
#include "src/editor/viewport/EditorViewportController.h"
#include "src/editor/viewport/TransformGizmoController.h"
#include "src/scene/SceneCloner.h"
#include "src/scene/SceneTransformSnapshot.h"
#include "src/resource/asset/AssetManager.h"
#include "src/resource/asset/MaterialAsset.h"
#include "src/resource/asset/Texture2DAsset.h"
#include "src/runtime/behavior/BehaviorRegistry.h"
#include "src/runtime/module/GameModule.h"
#include "src/pipeline/render/SceneRenderSnapshot.h"
#include "src/pipeline/shader/ShaderSourceAsset.h"

#include <nlohmann/json.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <winioctl.h>

namespace editor::validation
{
namespace
{
void Require(const bool Condition, const string_view Message)
{
	if (!Condition)
		throw std::runtime_error("Editor validation failed: " + string(Message));
}

struct JunctionReparseBuffer final
{
	uint32 ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	uint16 ReparseDataLength = 0;
	uint16 Reserved = 0;
	uint16 SubstituteNameOffset = 0;
	uint16 SubstituteNameLength = 0;
	uint16 PrintNameOffset = 0;
	uint16 PrintNameLength = 0;
	wchar_t PathBuffer[1]{};
};

void CreateValidationJunction(const std::filesystem::path &Link, const std::filesystem::path &Target)
{
	if (CreateDirectoryW(Link.c_str(), nullptr) == FALSE)
		throw std::runtime_error("Could not create validation junction directory");
	const std::wstring Substitute = L"\\??\\" + std::filesystem::absolute(Target).lexically_normal().native();
	const std::wstring Print = std::filesystem::absolute(Target).lexically_normal().native();
	const usize PathCharacters = Substitute.size() + 1U + Print.size() + 1U;
	const usize BufferBytes = offsetof(JunctionReparseBuffer, PathBuffer) + PathCharacters * sizeof(wchar_t);
	std::vector<uint8> Storage(BufferBytes);
	auto *Buffer = reinterpret_cast<JunctionReparseBuffer *>(Storage.data());
	Buffer->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	Buffer->SubstituteNameOffset = 0;
	Buffer->SubstituteNameLength = static_cast<uint16>(Substitute.size() * sizeof(wchar_t));
	Buffer->PrintNameOffset = static_cast<uint16>((Substitute.size() + 1U) * sizeof(wchar_t));
	Buffer->PrintNameLength = static_cast<uint16>(Print.size() * sizeof(wchar_t));
	std::memcpy(Buffer->PathBuffer, Substitute.c_str(), (Substitute.size() + 1U) * sizeof(wchar_t));
	std::memcpy(reinterpret_cast<uint8 *>(Buffer->PathBuffer) + Buffer->PrintNameOffset, Print.c_str(),
				(Print.size() + 1U) * sizeof(wchar_t));
	Buffer->ReparseDataLength = static_cast<uint16>(BufferBytes - offsetof(JunctionReparseBuffer, SubstituteNameOffset));

	const HANDLE Directory = CreateFileW(Link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
										 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (Directory == INVALID_HANDLE_VALUE)
	{
		(void)RemoveDirectoryW(Link.c_str());
		throw std::runtime_error("Could not open validation junction directory");
	}
	DWORD Returned = 0;
	const BOOL Applied =
		DeviceIoControl(Directory, FSCTL_SET_REPARSE_POINT, Buffer, Buffer->ReparseDataLength + 8U, nullptr, 0, &Returned, nullptr);
	(void)CloseHandle(Directory);
	if (Applied == FALSE)
	{
		(void)RemoveDirectoryW(Link.c_str());
		throw std::runtime_error("Could not apply validation junction reparse data");
	}
}

void ValidateSecurePaths()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLSecurePathValidation-" + util::UUID::GenerateRandomUUID().ToString());
	const std::filesystem::path Trusted = Root / "Trusted";
	const std::filesystem::path Outside = Root / "Outside";
	const std::filesystem::path Junction = Trusted / "Link";
	std::filesystem::create_directories(Trusted);
	std::filesystem::create_directories(Outside);
	struct Cleanup final
	{
		std::filesystem::path Junction;
		std::filesystem::path Root;
		~Cleanup()
		{
			(void)RemoveDirectoryW(this->Junction.c_str());
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Junction, Root};
	const std::array<uint8, 4> Payload{1U, 2U, 3U, 4U};
	core::io::SecurePath::WriteFileWithin(Outside, "Victim.bin", Payload, false, true, "secure-path validation victim");
	const string ShaderSource = "#version 460 core\nvoid main(){gl_Position=vec4(0.0);}\n";
	core::io::SecurePath::WriteFileWithin(Outside, "Outside.vert",
										  std::span(reinterpret_cast<const uint8 *>(ShaderSource.data()), ShaderSource.size()), false, true,
										  "secure-path validation shader");
	CreateValidationJunction(Junction, Outside);

	const auto Rejects = [](auto &&Operation)
	{
		try
		{
			std::forward<decltype(Operation)>(Operation)();
		}
		catch (const core::io::SecurePathException &)
		{
			return true;
		}
		return false;
	};
	Require(Rejects([&]() { (void)core::io::SecurePath::ReadFileWithin(Trusted, "Link/Victim.bin", 64U, "junction read"); }),
			"trusted-root reader traversed a directory junction");
	Require(Rejects([&]() { core::io::SecurePath::WriteFileWithin(Trusted, "Link/Injected.bin", Payload, false, true, "junction write"); }),
			"trusted-root writer traversed a directory junction");
	Require(Rejects([&]() { core::io::SecurePath::RemoveWithin(Trusted, "Link/Victim.bin", false, "junction delete"); }),
			"trusted-root deletion traversed a directory junction");
	Require(core::io::SecurePath::ReadFileWithin(Outside, "Victim.bin", 64U, "secure-path validation victim") ==
					std::vector<uint8>(Payload.begin(), Payload.end()) &&
				!std::filesystem::exists(Outside / "Injected.bin"),
			"junction rejection modified the external target");
	resource::AssetManager Assets(Trusted);
	Require(
		Rejects([&]()
				{ (void)Assets.GetAsset<pipeline::shader::ShaderSourceAsset>(resource::AssetType::ShaderSource, "Link/Outside.vert"); }),
		"asset manager traversed a directory junction below its trusted root");
	Require(
		Rejects(
			[&]()
			{ (void)Assets.GetAsset<pipeline::shader::ShaderSourceAsset>(resource::AssetType::ShaderSource, Outside / "Outside.vert"); }),
		"asset manager accepted an absolute asset path outside its trusted root");
}

void ValidateAuthoringContracts()
{
	document::SceneDocument Document("AuthoringContractsValidation");
	const util::UUID PersistentID = util::UUID::GenerateRandomUUID();
	const document::SceneObjectSpecification Specification{.Name = "Specified Object", .PersistentID = PersistentID};
	const world::ObjectHandle Object = Document.CreateObject(Specification);
	Require(Object.IsValid() && Document.GetScene().FindObject(PersistentID) == Object,
			"scene-object specification did not create an object with its requested stable identity");
	document::EditorSelection &Selection = Document.GetSelection();
	commands::EditorTransactionStack &History = Document.GetHistory();
	Require(Selection.Contains(PersistentID) && !History.HasOpenTransaction(),
			"authoring specification did not preserve selection or transaction state");
	{
		commands::EditorTransaction Transaction(History, "Authoring contract transaction");
		Require(Transaction.IsActive() && History.HasOpenTransaction(), "editor transaction did not open the command-history scope");
		Transaction.Commit();
	}
	Require(!History.HasOpenTransaction(), "editor transaction did not close its command-history scope");
	{
		commands::EditorTransaction Transaction(History, "Authoring rollback transaction");
		Transaction.Cancel();
	}
	Require(!History.HasOpenTransaction(), "editor transaction cancellation left an open command-history scope");

	const project::ProjectDescriptor ManagerDescriptor{
		.Name = "Project Manager Validation",
		.DescriptorPath = std::filesystem::temp_directory_path() /
						  ("OpenGLProjectManagerValidation-" + util::UUID::GenerateRandomUUID().ToString() + ".engineproject")};
	project::ProjectManager Manager(ManagerDescriptor);
	Require(Manager.IsOpen(), "project manager did not open its single owned project");
	bool DuplicateOpenRejected = false;
	try
	{
		Manager.Open(ManagerDescriptor);
	}
	catch (const std::logic_error &)
	{
		DuplicateOpenRejected = true;
	}
	Require(DuplicateOpenRejected, "project manager allowed a second project to replace the active project implicitly");
	Manager.Close();
	Require(!Manager.IsOpen(), "project manager close did not release the active project");
	bool ClosedAccessRejected = false;
	try
	{
		(void)Manager.GetProject();
	}
	catch (const std::logic_error &)
	{
		ClosedAccessRejected = true;
	}
	Require(ClosedAccessRejected, "project manager exposed a project after close");
	Manager.Open(ManagerDescriptor);
	Require(Manager.IsOpen(), "project manager could not reopen after close");
}

[[nodiscard]] util::UUID GetPersistentID(world::Scene &Scene, const world::ObjectHandle Object)
{
	const world::ComponentHandle<components::CObjectIdentityComponent> Identity =
		Scene.GetComponent<components::CObjectIdentityComponent>(Object);
	Require(Identity.IsValid(), "scene document object has no identity component");
	auto Access = Scene.Read();
	return Access.Resolve(Identity).GetPersistentID();
}

struct ValidationBehaviorState final
{
	uint32 UpdateCount = 0;
};

std::atomic<uint32> ValidationConstructs = 0;
std::atomic<uint32> ValidationStarts = 0;
std::atomic<uint32> ValidationUpdates = 0;
std::atomic<uint32> ValidationFixedUpdates = 0;
std::atomic<uint32> ValidationStops = 0;
std::atomic<uint32> ValidationDestroys = 0;

void ConstructValidationBehavior(void *State, runtime::behavior::BehaviorExecutionContext *Context)
{
	Require(Context != nullptr && Context->GetOwner().IsValid() && Context->GetInstanceID().IsValid(),
			"behavior construction context lost its stable identities");
	const components::BehaviorPropertyValue *Speed = Context->FindProperty("Speed");
	Require(Speed != nullptr && std::get_if<float32>(Speed) != nullptr && *std::get_if<float32>(Speed) == 2.0f,
			"behavior construction did not receive its authored property snapshot");
	::new (State) ValidationBehaviorState();
	ValidationConstructs.fetch_add(1, std::memory_order_relaxed);
}

void StartValidationBehavior(void *, runtime::behavior::BehaviorExecutionContext *)
{
	ValidationStarts.fetch_add(1, std::memory_order_relaxed);
}

void UpdateValidationBehavior(void *State, runtime::behavior::BehaviorExecutionContext *Context, const float32 DeltaSeconds)
{
	auto *Behavior = static_cast<ValidationBehaviorState *>(State);
	++Behavior->UpdateCount;
	ValidationUpdates.fetch_add(1, std::memory_order_relaxed);
	const world::ComponentHandle<components::CObjectTransformComponent> Transform =
		Context->GetScene().GetComponent<components::CObjectTransformComponent>(Context->GetOwner());
	auto Access = Context->GetScene().Write();
	components::CObjectTransformComponent &Component = Access.Resolve(Transform);
	Component.SetPosition(Component.GetPosition() + glm::vec3(DeltaSeconds, 0.0f, 0.0f));
}

void ThrowingValidationBehavior(void *, runtime::behavior::BehaviorExecutionContext *, float32)
{
	throw std::runtime_error("intentional validation failure");
}

void FixedUpdateValidationBehavior(void *, runtime::behavior::BehaviorExecutionContext *, float32)
{
	ValidationFixedUpdates.fetch_add(1, std::memory_order_relaxed);
}

void StopValidationBehavior(void *, runtime::behavior::BehaviorExecutionContext *)
{
	ValidationStops.fetch_add(1, std::memory_order_relaxed);
}

void DestroyValidationBehavior(void *State) noexcept
{
	static_cast<ValidationBehaviorState *>(State)->~ValidationBehaviorState();
	ValidationDestroys.fetch_add(1, std::memory_order_relaxed);
}

bool SerializeValidationBehavior(const void *State, std::byte *Destination, const usize Capacity, usize *WrittenSize, char *,
								 usize) noexcept
{
	if (State == nullptr || WrittenSize == nullptr)
		return false;
	*WrittenSize = sizeof(ValidationBehaviorState);
	if (Destination == nullptr)
		return Capacity == 0;
	if (Capacity < sizeof(ValidationBehaviorState))
		return false;
	std::memcpy(Destination, State, sizeof(ValidationBehaviorState));
	return true;
}

bool RestoreValidationBehavior(void *State, const std::byte *Source, const usize Size, const uint32 SourceSchemaVersion,
							   runtime::behavior::BehaviorExecutionContext *, char *, usize) noexcept
{
	if (State == nullptr || Source == nullptr || SourceSchemaVersion != 1 || Size != sizeof(ValidationBehaviorState))
		return false;
	std::memcpy(State, Source, sizeof(ValidationBehaviorState));
	return true;
}

bool MigrateValidationProperties(const uint32 SourceSchemaVersion,
								 std::unordered_map<string, components::BehaviorPropertyValue> *Properties, char *, usize) noexcept
{
	if (SourceSchemaVersion != 1 || Properties == nullptr)
		return false;
	Properties->insert_or_assign("Migrated", true);
	return true;
}

[[nodiscard]] runtime::behavior::BehaviorDescriptor MakeValidationBehaviorDescriptor(const components::BehaviorTypeID Type, string Name,
																					 const bool Throws)
{
	return {.Type = Type,
			.Name = std::move(Name),
			.SchemaVersion = 1,
			.StateSize = sizeof(ValidationBehaviorState),
			.StateAlignment = alignof(ValidationBehaviorState),
			.ParallelUpdateSafe = true,
			.Properties = {{.Name = "Speed", .DefaultValue = float32{2.0f}}},
			.Construct = &ConstructValidationBehavior,
			.Start = &StartValidationBehavior,
			.Update = Throws ? &ThrowingValidationBehavior : &UpdateValidationBehavior,
			.FixedUpdate = &FixedUpdateValidationBehavior,
			.Stop = &StopValidationBehavior,
			.Destroy = &DestroyValidationBehavior,
			.SerializeState = &SerializeValidationBehavior,
			.RestoreState = &RestoreValidationBehavior};
}

void ValidateViewportSelection()
{
	document::SceneDocument Document("ViewportSelectionValidation");
	const world::ObjectHandle FirstObject = Document.CreateObject("First");
	const world::ObjectHandle SecondObject = Document.CreateObject("Second");
	const util::UUID FirstID = GetPersistentID(Document.GetScene(), FirstObject);
	const util::UUID SecondID = GetPersistentID(Document.GetScene(), SecondObject);

	viewport::EditorViewportController Controller;
	const pipeline::render::PickRequestID ReplaceRequest = Controller.QueuePick(0.25f, 0.75f, viewport::SelectionOperation::Replace);
	std::vector<viewport::ViewportPickRequest> Requests = Controller.CollectPickRequests({100, 200});
	Require(Requests.size() == 1 && Requests.front().Request == ReplaceRequest && Requests.front().X == 25 && Requests.front().Y == 49,
			"normalized viewport pick did not convert from top-left UI coordinates to bottom-left texture coordinates");

	viewport::EditorViewportFrame ReplaceFrame;
	ReplaceFrame.CompletedPicks.push_back({.Request = ReplaceRequest, .Object = FirstObject, .SourceFrame = 1});
	Controller.ApplyFrame(Document, ReplaceFrame);
	Require(Document.GetSelection().Size() == 1 && Document.GetSelection().GetPrimary() == FirstID,
			"replace selection did not select the resolved persistent object identity");

	const pipeline::render::PickRequestID DeferredRequest = Controller.QueuePick(1.0f, 1.0f, viewport::SelectionOperation::Add);
	Requests = Controller.CollectPickRequests({100, 200});
	Require(Requests.size() == 1 && Requests.front().X == 99 && Requests.front().Y == 0,
			"inclusive normalized viewport edge did not clamp to the final texture pixel");
	viewport::EditorViewportFrame DeferredFrame;
	DeferredFrame.DeferredPicks.push_back(DeferredRequest);
	Controller.ApplyFrame(Document, DeferredFrame);
	Requests = Controller.CollectPickRequests({64, 32});
	Require(Requests.size() == 1 && Requests.front().Request == DeferredRequest && Requests.front().X == 63 && Requests.front().Y == 0,
			"deferred viewport pick was not retried against the current resized extent");

	viewport::EditorViewportFrame AddFrame;
	AddFrame.CompletedPicks.push_back({.Request = DeferredRequest, .Object = SecondObject, .SourceFrame = 2});
	Controller.ApplyFrame(Document, AddFrame);
	Require(Document.GetSelection().Size() == 2 && Document.GetSelection().GetPrimary() == SecondID,
			"add selection did not preserve the existing selection and update the primary identity");

	const pipeline::render::PickRequestID ToggleRequest = Controller.QueuePick(0.5f, 0.5f, viewport::SelectionOperation::Toggle);
	(void)Controller.CollectPickRequests({32, 32});
	viewport::EditorViewportFrame ToggleFrame;
	ToggleFrame.CompletedPicks.push_back({.Request = ToggleRequest, .Object = FirstObject, .SourceFrame = 3});
	Controller.ApplyFrame(Document, ToggleFrame);
	Require(Document.GetSelection().Size() == 1 && Document.GetSelection().GetPrimary() == SecondID,
			"toggle selection did not remove an already selected identity");

	const world::ObjectHandle StaleObject = Document.CreateObject("Stale");
	const util::UUID StaleID = GetPersistentID(Document.GetScene(), StaleObject);
	const pipeline::render::PickRequestID StaleRequest = Controller.QueuePick(0.5f, 0.5f, viewport::SelectionOperation::Replace);
	(void)Controller.CollectPickRequests({32, 32});
	Document.DestroyObject(StaleID);
	viewport::EditorViewportFrame StaleFrame;
	StaleFrame.CompletedPicks.push_back({.Request = StaleRequest, .Object = StaleObject, .SourceFrame = 4});
	Controller.ApplyFrame(Document, StaleFrame);
	Require(Document.GetSelection().Empty(), "a completed pick selected an object handle destroyed before readback completion");

	const pipeline::render::PickRequestID BackgroundRequest = Controller.QueuePick(0.0f, 0.0f, viewport::SelectionOperation::Replace);
	(void)Controller.CollectPickRequests({32, 32});
	viewport::EditorViewportFrame BackgroundFrame;
	BackgroundFrame.CompletedPicks.push_back({.Request = BackgroundRequest, .Object = std::nullopt, .SourceFrame = 5});
	Controller.ApplyFrame(Document, BackgroundFrame);
	Require(Document.GetSelection().Empty() && Controller.GetPendingPickCount() == 0,
			"background replacement pick did not leave a clean empty selection and request queue");

	const auto LockedIdentity = Document.GetScene().GetComponent<components::CObjectIdentityComponent>(SecondObject);
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(LockedIdentity).SetLocked(true);
	}
	const pipeline::render::PickRequestID LockedRequest = Controller.QueuePick(0.5f, 0.5f, viewport::SelectionOperation::Replace);
	(void)Controller.CollectPickRequests({32, 32});
	viewport::EditorViewportFrame LockedFrame;
	LockedFrame.CompletedPicks.push_back({.Request = LockedRequest, .Object = SecondObject, .SourceFrame = 6});
	Controller.ApplyFrame(Document, LockedFrame);
	Require(Document.GetSelection().Empty(), "viewport picking selected an editor-locked object");
}

void ValidateComponentReflection()
{
	reflection::ReflectionRegistry Registry;
	reflection::RegisterCoreComponentReflection(Registry);
	Require(Registry.Snapshot().size() == components::CObjectComponents,
			"core component reflection did not register every concrete component type");

	const std::optional<reflection::TypeDescriptor> TransformDescriptor = Registry.Find("components.CObjectTransformComponent");
	Require(TransformDescriptor.has_value(), "transform component reflection descriptor is missing");
	const auto PositionProperty = std::find_if(TransformDescriptor->Properties.begin(), TransformDescriptor->Properties.end(),
											   [](const reflection::PropertyDescriptor &Property) { return Property.Name == "Position"; });
	Require(PositionProperty != TransformDescriptor->Properties.end() && PositionProperty->Write,
			"transform position reflection property is missing or read-only");
	Require(PositionProperty->DefaultValue.has_value() && std::get<glm::vec3>(*PositionProperty->DefaultValue) == glm::vec3(0.0f),
			"transform position reflection did not expose its reset value");

	document::SceneDocument Document("ReflectionValidation");
	const world::ObjectHandle Object = Document.CreateObject("Reflected");
	const world::ComponentHandle<components::CObjectTransformComponent> Transform =
		Document.GetScene().GetComponent<components::CObjectTransformComponent>(Object);
	{
		auto Access = Document.GetScene().Write();
		components::CObjectTransformComponent &Component = Access.Resolve(Transform);
		PositionProperty->Write(&Component, reflection::PropertyValue(glm::vec3(3.0f, 4.0f, 5.0f)), {.Scene = &Document.GetScene()});
		Require(std::get<glm::vec3>(PositionProperty->Read(&Component)) == glm::vec3(3.0f, 4.0f, 5.0f),
				"reflected transform edit bypassed or failed the component setter");
	}

	Document.Execute(commands::PropertyEditCommand::Create(Document.GetScene(), Object, components::CObjectTransformComponent::TypeID,
														   *PositionProperty, reflection::PropertyValue(glm::vec3(6.0f, 7.0f, 8.0f))));
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Transform).GetPosition() == glm::vec3(6.0f, 7.0f, 8.0f),
				"property edit command did not execute through the reflected component setter");
	}
	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Transform).GetPosition() == glm::vec3(3.0f, 4.0f, 5.0f),
				"property edit command did not restore its captured reflected value");
	}
	Document.Redo();
	Document.Execute(commands::PropertyEditCommand::Create(Document.GetScene(), Object, components::CObjectTransformComponent::TypeID,
														   *PositionProperty, reflection::PropertyValue(glm::vec3(9.0f, 10.0f, 11.0f))));
	Document.Execute(commands::PropertyEditCommand::Create(Document.GetScene(), Object, components::CObjectTransformComponent::TypeID,
														   *PositionProperty, reflection::PropertyValue(glm::vec3(12.0f, 13.0f, 14.0f))));
	Require(Document.GetHistory().GetUndoCount() == 1, "continuous reflected edits did not merge into one undo operation");
	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Transform).GetPosition() == glm::vec3(3.0f, 4.0f, 5.0f),
				"merged reflected edits did not restore the value before the edit sequence");
	}

	const world::ObjectHandle SecondObject = Document.CreateObject("Second Reflected");
	const auto SecondTransform = Document.GetScene().GetComponent<components::CObjectTransformComponent>(SecondObject);
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(SecondTransform).SetPosition(glm::vec3(20.0f, 21.0f, 22.0f));
	}
	const std::array Targets{Object, SecondObject};
	Document.Execute(commands::PropertyEditCommand::Create(Document.GetScene(), Targets, components::CObjectTransformComponent::TypeID,
														   *PositionProperty, reflection::PropertyValue(glm::vec3(30.0f, 31.0f, 32.0f))));
	Document.Execute(commands::PropertyEditCommand::Create(Document.GetScene(), Targets, components::CObjectTransformComponent::TypeID,
														   *PositionProperty, reflection::PropertyValue(glm::vec3(40.0f, 41.0f, 42.0f))));
	Require(Document.GetHistory().GetUndoCount() == 1, "continuous multi-object property edits did not merge into one undo operation");
	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Transform).GetPosition() == glm::vec3(3.0f, 4.0f, 5.0f) &&
					Access.Resolve(SecondTransform).GetPosition() == glm::vec3(20.0f, 21.0f, 22.0f),
				"multi-object property undo did not restore each target's distinct original value");
	}

	const std::optional<reflection::TypeDescriptor> CameraDescriptor = Registry.Find("components.CObjectCameraComponent");
	Require(CameraDescriptor.has_value(), "camera component reflection descriptor is missing");
	const auto ProjectionProperty =
		std::find_if(CameraDescriptor->Properties.begin(), CameraDescriptor->Properties.end(),
					 [](const reflection::PropertyDescriptor &Property) { return Property.Name == "Projection"; });
	Require(ProjectionProperty != CameraDescriptor->Properties.end() && ProjectionProperty->EnumOptions.size() == 2,
			"camera projection reflection did not retain its enum options");

	document::SceneDocument BehaviorDocument("BehaviorCommandValidation");
	const world::ObjectHandle BehaviorObject = BehaviorDocument.CreateObject("Behavior Host");
	const auto BehaviorComponent = BehaviorDocument.GetScene().AddComponent<components::CObjectBehaviorComponent>(BehaviorObject);
	const runtime::behavior::BehaviorDescriptor BehaviorDescriptor = MakeValidationBehaviorDescriptor(900, "EditableBehavior", false);
	BehaviorDocument.Execute(
		std::make_unique<commands::AddBehaviorCommand>(BehaviorDocument.GetScene(), BehaviorObject, BehaviorDescriptor));
	util::UUID BehaviorInstanceID;
	{
		auto Access = BehaviorDocument.GetScene().Read();
		const auto &Behaviors = Access.Resolve(BehaviorComponent).GetBehaviors();
		Require(Behaviors.size() == 1 && Behaviors.front().Type == 900,
				"add behavior command did not attach the registered project behavior");
		BehaviorInstanceID = Behaviors.front().InstanceID;
	}
	BehaviorDocument.Undo();
	{
		auto Access = BehaviorDocument.GetScene().Read();
		Require(Access.Resolve(BehaviorComponent).GetBehaviors().empty(), "add behavior undo did not detach its behavior instance");
	}
	BehaviorDocument.Redo();
	components::BehaviorInstance EditedBehavior;
	{
		auto Access = BehaviorDocument.GetScene().Read();
		EditedBehavior = Access.Resolve(BehaviorComponent).FindBehavior(BehaviorInstanceID).value();
	}
	EditedBehavior.Enabled = false;
	EditedBehavior.Properties.insert_or_assign("Speed", float32{4.0f});
	BehaviorDocument.Execute(std::make_unique<commands::EditBehaviorCommand>(BehaviorDocument.GetScene(), BehaviorObject, EditedBehavior));
	BehaviorDocument.Execute(
		std::make_unique<commands::RemoveBehaviorCommand>(BehaviorDocument.GetScene(), BehaviorObject, BehaviorInstanceID));
	BehaviorDocument.Undo();
	{
		auto Access = BehaviorDocument.GetScene().Read();
		const std::optional<components::BehaviorInstance> Restored = Access.Resolve(BehaviorComponent).FindBehavior(BehaviorInstanceID);
		Require(Restored.has_value() && !Restored->Enabled && std::get<float32>(Restored->Properties.at("Speed")) == 4.0f,
				"remove behavior undo did not restore the complete edited behavior instance");
	}
}

void ValidateTransformEditing()
{
	document::SceneDocument Document("TransformValidation");
	const world::ObjectHandle First = Document.CreateObject("First");
	const world::ObjectHandle Second = Document.CreateObject("Second");
	const util::UUID FirstID = GetPersistentID(Document.GetScene(), First);
	const util::UUID SecondID = GetPersistentID(Document.GetScene(), Second);
	Document.GetSelection().SelectOnly(FirstID);
	Document.GetSelection().Add(SecondID);

	const auto SetPosition = [&Document](const world::ObjectHandle Object, const glm::vec3 Position)
	{
		const world::ComponentHandle<components::CObjectTransformComponent> Transform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(Object);
		auto Access = Document.GetScene().Write();
		Access.Resolve(Transform).SetPosition(Position);
	};
	const auto GetPosition = [&Document](const world::ObjectHandle Object)
	{
		const world::ComponentHandle<components::CObjectTransformComponent> Transform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(Object);
		auto Access = Document.GetScene().Read();
		return Access.Resolve(Transform).GetPosition();
	};
	const auto GetScale = [&Document](const world::ObjectHandle Object)
	{
		const world::ComponentHandle<components::CObjectTransformComponent> Transform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(Object);
		auto Access = Document.GetScene().Read();
		return Access.Resolve(Transform).GetScale();
	};
	SetPosition(First, glm::vec3(0.0f));
	SetPosition(Second, glm::vec3(2.0f, 0.0f, 0.0f));

	const std::array Targets{
		commands::TransformEditTarget{
			.Object = First, .Before = {.Position = glm::vec3(0.0f)}, .After = {.Position = glm::vec3(1.0f, 2.0f, 3.0f)}},
		commands::TransformEditTarget{
			.Object = Second, .Before = {.Position = glm::vec3(2.0f, 0.0f, 0.0f)}, .After = {.Position = glm::vec3(3.0f, 2.0f, 3.0f)}}};
	Document.Execute(commands::TransformEditCommand::Create(Document.GetScene(), Targets));
	Require(GetPosition(First) == glm::vec3(1.0f, 2.0f, 3.0f) && GetPosition(Second) == glm::vec3(3.0f, 2.0f, 3.0f),
			"multi-object transform command did not atomically apply every target");
	Document.Undo();
	Require(GetPosition(First) == glm::vec3(0.0f) && GetPosition(Second) == glm::vec3(2.0f, 0.0f, 0.0f),
			"multi-object transform command did not restore every captured transform");
	Document.Redo();

	Camera Camera(0.1f, 60.0f, 0.05f, 10'000.0f);
	Camera.Position = glm::vec3(0.0f, 0.0f, 10.0f);
	Camera.Yaw = -90.0f;
	Camera.Pitch = 0.0f;
	Camera.UpdateCameraVectors();
	viewport::TransformGizmoController Gizmo;
	Gizmo.SetPivot(viewport::TransformGizmoPivot::MedianPoint);
	const glm::vec3 FirstBeforeDrag = GetPosition(First);
	const glm::vec3 SecondBeforeDrag = GetPosition(Second);
	Require(Gizmo.BeginDrag(Document, Camera, {1'000, 1'000}, 0.5f, 0.5f, viewport::TransformGizmoHandle::AxisX),
			"translation gizmo did not begin for a valid selected transform set");
	Require(Gizmo.UpdateDrag(Camera, {1'000, 1'000}, 0.6f, 0.5f), "translation gizmo rejected an active drag update");
	const viewport::TransformGizmoVisualState LiveDragVisualState = Gizmo.BuildVisualState(Document, Camera);
	const glm::vec3 ExpectedLiveDragPivot = (GetPosition(First) + GetPosition(Second)) * 0.5f;
	Require(LiveDragVisualState.Dragging && glm::distance(LiveDragVisualState.Pivot, ExpectedLiveDragPivot) < 1.0e-4f &&
				glm::distance(LiveDragVisualState.Pivot, (FirstBeforeDrag + SecondBeforeDrag) * 0.5f) > 1.0e-4f,
			"transform-gizmo handles did not follow the selection during an active translation drag");
	Gizmo.CommitDrag();
	const glm::vec3 FirstAfterDrag = GetPosition(First);
	const glm::vec3 SecondAfterDrag = GetPosition(Second);
	const float32 FirstDelta = FirstAfterDrag.x - FirstBeforeDrag.x;
	const float32 SecondDelta = SecondAfterDrag.x - SecondBeforeDrag.x;
	Require(FirstDelta > 0.0f && std::abs(FirstDelta - SecondDelta) < 1.0e-4f,
			"translation gizmo did not preserve a rigid multi-selection offset");
	Document.Undo();
	Require(glm::distance(GetPosition(First), FirstBeforeDrag) < 1.0e-4f && glm::distance(GetPosition(Second), SecondBeforeDrag) < 1.0e-4f,
			"translation gizmo did not publish one undoable drag transaction");

	viewport::EditorCameraController CameraController;
	Require(CameraController.FocusSelection(Document, Camera), "editor camera could not focus a valid transform selection");
	const glm::vec3 ExpectedFocusCenter = (GetPosition(First) + GetPosition(Second)) * 0.5f;
	Require(glm::distance(CameraController.GetOrbitPivot(), ExpectedFocusCenter) < 1.0e-4f &&
				glm::distance(Camera.Position, ExpectedFocusCenter) > 0.0f,
			"editor camera focus did not frame the current selection around its shared center");
	const core::input::InputSnapshot EmptyCameraInput;
	const viewport::EditorCameraInteraction HeldFlyNavigation =
		CameraController.Update(Camera, EmptyCameraInput, 1.0f / 60.0f, {.RightMouseDown = true}, true, true, true, false, false, false);
	Require(HeldFlyNavigation.WantsRelativePointer, "buffered held right-mouse input did not engage editor mouse-look");
	const viewport::EditorCameraInteraction ReleasedFlyNavigation =
		CameraController.Update(Camera, EmptyCameraInput, 1.0f / 60.0f, {}, true, true, true, false, false, false);
	Require(!ReleasedFlyNavigation.WantsRelativePointer, "editor mouse-look remained active after right mouse was released");

	const viewport::TransformGizmoVisualState VisualState = Gizmo.BuildVisualState(Document, Camera);
	const glm::vec3 AxisSample = VisualState.Pivot + glm::normalize(VisualState.Basis[0]) * VisualState.WorldScale * 0.7f;
	glm::vec4 AxisClip = Camera.GetProjectionMatrix({1'000, 1'000}) * Camera.GetViewMatrix() * glm::vec4(AxisSample, 1.0f);
	AxisClip /= AxisClip.w;
	const float32 AxisX = AxisClip.x * 0.5f + 0.5f;
	const float32 AxisYFromTop = 0.5f - AxisClip.y * 0.5f;
	Require(Gizmo.HitTest(Document, Camera, {1'000, 1'000}, AxisX, AxisYFromTop) == viewport::TransformGizmoHandle::AxisX,
			"transform-gizmo hit test did not resolve its visible X-axis handle");
	Gizmo.SetOperation(viewport::TransformGizmoOperation::Scale);
	glm::vec4 PivotClip = Camera.GetProjectionMatrix({1'000, 1'000}) * Camera.GetViewMatrix() * glm::vec4(VisualState.Pivot, 1.0f);
	PivotClip /= PivotClip.w;
	Require(Gizmo.HitTest(Document, Camera, {1'000, 1'000}, PivotClip.x * 0.5f + 0.5f, 0.5f - PivotClip.y * 0.5f) ==
				viewport::TransformGizmoHandle::Uniform,
			"transform-gizmo hit test did not resolve the visible uniform-scale handle");
	Gizmo.SetOperation(viewport::TransformGizmoOperation::Universal);
	const viewport::TransformGizmoVisualState UniversalState = Gizmo.BuildVisualState(Document, Camera);
	const glm::vec3 UniversalScaleSample =
		UniversalState.Pivot + glm::normalize(UniversalState.Basis[0]) * UniversalState.WorldScale * 0.72f;
	glm::vec4 UniversalScaleClip =
		Camera.GetProjectionMatrix({1'000, 1'000}) * Camera.GetViewMatrix() * glm::vec4(UniversalScaleSample, 1.0f);
	UniversalScaleClip /= UniversalScaleClip.w;
	const float32 UniversalScaleX = UniversalScaleClip.x * 0.5f + 0.5f;
	const float32 UniversalScaleY = 0.5f - UniversalScaleClip.y * 0.5f;
	const viewport::TransformGizmoHandle UniversalScaleHandle =
		Gizmo.HitTest(Document, Camera, {1'000, 1'000}, UniversalScaleX, UniversalScaleY);
	Require(UniversalScaleHandle == viewport::TransformGizmoHandle::ScaleAxisX,
			"universal transform gizmo did not distinguish its X-axis scale handle");
	const glm::vec3 ScaleBeforeUniversalDrag = GetScale(First);
	Require(Gizmo.BeginDrag(Document, Camera, {1'000, 1'000}, UniversalScaleX, UniversalScaleY, UniversalScaleHandle),
			"universal transform gizmo did not begin an encoded scale drag");
	Require(Gizmo.UpdateDrag(Camera, {1'000, 1'000}, UniversalScaleX + 0.05f, UniversalScaleY),
			"universal transform gizmo rejected its encoded scale update");
	Gizmo.CommitDrag();
	Require(std::abs(GetScale(First).x - ScaleBeforeUniversalDrag.x) > 1.0e-4f,
			"universal transform gizmo routed an encoded scale handle to the wrong operation");
	Document.Undo();
	Require(glm::distance(GetScale(First), ScaleBeforeUniversalDrag) < 1.0e-4f,
			"universal transform gizmo did not publish one undoable scale transaction");

	Gizmo.SetOperation(viewport::TransformGizmoOperation::Scale);
	Gizmo.SetPivot(viewport::TransformGizmoPivot::IndividualOrigins);
	const viewport::TransformGizmoVisualState IndividualState = Gizmo.BuildVisualState(Document, Camera);
	glm::vec4 IndividualPivotClip =
		Camera.GetProjectionMatrix({1'000, 1'000}) * Camera.GetViewMatrix() * glm::vec4(IndividualState.Pivot, 1.0f);
	IndividualPivotClip /= IndividualPivotClip.w;
	const float32 IndividualPivotX = IndividualPivotClip.x * 0.5f + 0.5f;
	const float32 IndividualPivotY = 0.5f - IndividualPivotClip.y * 0.5f;
	const glm::vec3 FirstBeforeIndividualScale = GetPosition(First);
	const glm::vec3 SecondBeforeIndividualScale = GetPosition(Second);
	const glm::vec3 FirstScaleBeforeIndividualScale = GetScale(First);
	Require(Gizmo.BeginDrag(Document, Camera, {1'000, 1'000}, IndividualPivotX, IndividualPivotY, viewport::TransformGizmoHandle::Uniform),
			"individual-origins scale gizmo did not begin");
	Require(Gizmo.UpdateDrag(Camera, {1'000, 1'000}, IndividualPivotX, IndividualPivotY - 0.05f),
			"individual-origins scale gizmo rejected an active update");
	Gizmo.CommitDrag();
	Require(glm::distance(GetPosition(First), FirstBeforeIndividualScale) < 1.0e-4f,
			"individual-origins scaling moved the first object origin");
	Require(glm::distance(GetPosition(Second), SecondBeforeIndividualScale) < 1.0e-4f,
			"individual-origins scaling moved the second object origin");
	Require(glm::distance(GetScale(First), FirstScaleBeforeIndividualScale) > 1.0e-4f,
			"individual-origins scaling did not scale the first selected object");
	Document.Undo();

	const glm::vec3 FirstScaleBeforeSingularDrag = GetScale(First);
	const glm::vec3 SecondScaleBeforeSingularDrag = GetScale(Second);
	Require(Gizmo.BeginDrag(Document, Camera, {1'000, 1'000}, IndividualPivotX, IndividualPivotY, viewport::TransformGizmoHandle::Uniform),
			"minimum-scale regression gizmo did not begin");
	Require(Gizmo.UpdateDrag(Camera, {1'000, 1'000}, IndividualPivotX, 0.99f), "minimum-scale regression gizmo rejected its active drag");
	Gizmo.CommitDrag();
	const glm::vec3 FirstMinimumScale = GetScale(First);
	const glm::vec3 SecondMinimumScale = GetScale(Second);
	Require(std::isfinite(FirstMinimumScale.x) && std::isfinite(FirstMinimumScale.y) && std::isfinite(FirstMinimumScale.z) &&
				std::isfinite(SecondMinimumScale.x) && std::isfinite(SecondMinimumScale.y) && std::isfinite(SecondMinimumScale.z) &&
				glm::all(glm::greaterThan(FirstMinimumScale, glm::vec3(1.0e-4f))) &&
				glm::all(glm::greaterThan(SecondMinimumScale, glm::vec3(1.0e-4f))),
			"scaling through zero published a degenerate transform");
	Document.Undo();
	Require(glm::distance(GetScale(First), FirstScaleBeforeSingularDrag) < 1.0e-4f &&
				glm::distance(GetScale(Second), SecondScaleBeforeSingularDrag) < 1.0e-4f,
			"minimum-scale drag did not remain one reversible multi-object transaction");

	Gizmo.SetOperation(viewport::TransformGizmoOperation::Translate);
	Gizmo.SetPivot(viewport::TransformGizmoPivot::MedianPoint);
	const viewport::TransformGizmoVisualState CancellationState = Gizmo.BuildVisualState(Document, Camera);
	glm::vec4 CancellationPivotClip =
		Camera.GetProjectionMatrix({1'000, 1'000}) * Camera.GetViewMatrix() * glm::vec4(CancellationState.Pivot, 1.0f);
	CancellationPivotClip /= CancellationPivotClip.w;
	const float32 CancellationPivotX = CancellationPivotClip.x * 0.5f + 0.5f;
	const float32 CancellationPivotY = 0.5f - CancellationPivotClip.y * 0.5f;
	const glm::vec3 FirstBeforeCancellation = GetPosition(First);
	const glm::vec3 SecondBeforeCancellation = GetPosition(Second);
	Require(
		Gizmo.BeginDrag(Document, Camera, {1'000, 1'000}, CancellationPivotX, CancellationPivotY, viewport::TransformGizmoHandle::AxisX),
		"cancellable transform-gizmo drag did not begin");
	Require(Gizmo.UpdateDrag(Camera, {1'000, 1'000}, CancellationPivotX + 0.05f, CancellationPivotY),
			"cancellable transform-gizmo drag rejected an active update");
	Gizmo.CancelDrag();
	Require(glm::distance(GetPosition(First), FirstBeforeCancellation) < 1.0e-4f &&
				glm::distance(GetPosition(Second), SecondBeforeCancellation) < 1.0e-4f,
			"transform-gizmo cancellation did not restore the exact pre-drag selection state");

	{
		document::SceneDocument HierarchicalDocument("HierarchicalTransformValidation");
		const world::ObjectHandle ParentA = HierarchicalDocument.CreateObject("Parent A");
		const world::ObjectHandle ParentB = HierarchicalDocument.CreateObject("Parent B");
		const world::ObjectHandle ChildA = HierarchicalDocument.CreateObject("Child A", ParentA);
		const world::ObjectHandle ChildB = HierarchicalDocument.CreateObject("Child B", ParentB);
		const auto SetTransform = [&HierarchicalDocument](const world::ObjectHandle Object, const glm::vec3 Position,
														  const glm::quat Rotation, const glm::vec3 Scale)
		{
			const auto Transform = HierarchicalDocument.GetScene().GetComponent<components::CObjectTransformComponent>(Object);
			auto Access = HierarchicalDocument.GetScene().Write();
			Access.Resolve(Transform).SetTransform(Position, Rotation, Scale);
		};
		SetTransform(ParentA, {-2.0f, 0.0f, 0.0f}, glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(2.0f));
		SetTransform(ParentB, {2.0f, 0.0f, 0.0f}, glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(0.5f));
		HierarchicalDocument.GetSelection().SelectOnly(GetPersistentID(HierarchicalDocument.GetScene(), ChildA));
		HierarchicalDocument.GetSelection().Add(GetPersistentID(HierarchicalDocument.GetScene(), ChildB));
		const auto CaptureWorldPositions = [&HierarchicalDocument, ChildA, ChildB]()
		{
			auto Access = HierarchicalDocument.GetScene().Read();
			const world::SceneTransformSnapshot Snapshot = world::SceneTransformSnapshot::Build(Access);
			return std::array{Snapshot.GetPosition(ChildA), Snapshot.GetPosition(ChildB)};
		};
		const std::array WorldBefore = CaptureWorldPositions();
		viewport::TransformGizmoController HierarchicalGizmo;
		HierarchicalGizmo.SetOperation(viewport::TransformGizmoOperation::Translate);
		HierarchicalGizmo.SetSpace(viewport::TransformGizmoSpace::World);
		HierarchicalGizmo.SetPivot(viewport::TransformGizmoPivot::MedianPoint);
		Require(
			HierarchicalGizmo.BeginDrag(HierarchicalDocument, Camera, {1'000, 1'000}, 0.5f, 0.5f, viewport::TransformGizmoHandle::AxisX),
			"hierarchy-aware transform gizmo did not begin a valid multi-parent drag");
		Require(HierarchicalGizmo.UpdateDrag(Camera, {1'000, 1'000}, 0.6f, 0.5f),
				"hierarchy-aware transform gizmo rejected a valid multi-parent update");
		HierarchicalGizmo.CommitDrag();
		const std::array WorldAfter = CaptureWorldPositions();
		const glm::vec3 DeltaA = WorldAfter[0] - WorldBefore[0];
		const glm::vec3 DeltaB = WorldAfter[1] - WorldBefore[1];
		Require(DeltaA.x > 0.0f && glm::distance(DeltaA, DeltaB) < 1.0e-4f && std::abs(DeltaA.y) < 1.0e-4f && std::abs(DeltaA.z) < 1.0e-4f,
				"world-space multi-selection drag did not compensate for distinct parent transforms");
		HierarchicalDocument.Undo();
		const std::array WorldRestored = CaptureWorldPositions();
		Require(glm::distance(WorldRestored[0], WorldBefore[0]) < 1.0e-4f && glm::distance(WorldRestored[1], WorldBefore[1]) < 1.0e-4f,
				"hierarchy-aware transform drag did not restore both world transforms through one undo");
	}

	{
		const auto Identity = Document.GetScene().GetComponent<components::CObjectIdentityComponent>(First);
		auto Access = Document.GetScene().Write();
		Access.Resolve(Identity).SetLocked(true);
	}
	bool LockedTransformRejected = false;
	try
	{
		const std::array LockedTarget{commands::TransformEditTarget{
			.Object = First, .Before = {.Position = GetPosition(First)}, .After = {.Position = glm::vec3(100.0f)}}};
		(void)commands::TransformEditCommand::Create(Document.GetScene(), LockedTarget);
	}
	catch (const world::SceneException &)
	{
		LockedTransformRejected = true;
	}
	Require(LockedTransformRejected, "transform command accepted an editor-locked object");
	Require(!Gizmo.BuildVisualState(Document, Camera).Visible,
			"transform gizmo remained visible while the selected set contained a locked object");
}

void ValidateSceneHierarchy()
{
	document::SceneDocument Document("HierarchyValidation");
	const world::ObjectHandle Root = Document.CreateObject("Root");
	const util::UUID RootID = GetPersistentID(Document.GetScene(), Root);
	const world::ObjectHandle ChildB = Document.CreateObject("Child B", Root);
	const world::ObjectHandle ChildA = Document.CreateObject("Child A", Root);
	const util::UUID ChildAID = GetPersistentID(Document.GetScene(), ChildA);
	const util::UUID ChildBID = GetPersistentID(Document.GetScene(), ChildB);
	Document.SetParent(ChildAID, RootID, 1);
	Document.SetParent(ChildBID, RootID, 2);
	{
		const auto RootTransform = Document.GetScene().GetComponent<components::CObjectTransformComponent>(Root);
		const auto ChildTransform = Document.GetScene().GetComponent<components::CObjectTransformComponent>(ChildA);
		const auto ChildIdentity = Document.GetScene().GetComponent<components::CObjectIdentityComponent>(ChildA);
		auto Access = Document.GetScene().Write();
		Access.Resolve(RootTransform).SetPosition({10.0f, 0.0f, 0.0f});
		Access.Resolve(ChildTransform).SetPosition({1.0f, 2.0f, 3.0f});
		Access.Resolve(ChildIdentity).SetTags({"SpecialGameplayTag"});
	}
	{
		auto Access = Document.GetScene().Read();
		const world::SceneTransformSnapshot Transforms = world::SceneTransformSnapshot::Build(Access);
		Require(glm::length(Transforms.GetPosition(ChildA) - glm::vec3(11.0f, 2.0f, 3.0f)) < 1.0e-5f,
				"scene transform snapshot did not compose parent and child transforms");
	}

	const hierarchy::SceneHierarchySnapshot Snapshot = hierarchy::SceneHierarchyBuilder::Build(Document.GetScene(), Document.GetRevision());
	Require(Snapshot.SceneRevision == Document.GetRevision() && Snapshot.Rows.size() == 3,
			"scene hierarchy snapshot lost its document revision or object rows");
	Require(Snapshot.Rows[0].PersistentID == RootID && Snapshot.Rows[0].Depth == 0 && Snapshot.Rows[0].ChildCount == 2 &&
				Snapshot.Rows[1].PersistentID == ChildAID && Snapshot.Rows[1].ParentRow == 0 && Snapshot.Rows[1].Depth == 1 &&
				Snapshot.Rows[2].PersistentID == ChildBID,
			"scene hierarchy snapshot did not preserve parent depth and sibling order");

	const hierarchy::SceneHierarchySnapshot Filtered = hierarchy::SceneHierarchyBuilder::Filter(Snapshot, "child b");
	Require(Filtered.Rows.size() == 2 && Filtered.Rows[0].PersistentID == RootID && Filtered.Rows[0].ChildCount == 1 &&
				Filtered.Rows[1].PersistentID == ChildBID && Filtered.Rows[1].ParentRow == 0,
			"scene hierarchy filter did not retain the matching row and its ancestor chain");
	const hierarchy::SceneHierarchySnapshot TagFiltered = hierarchy::SceneHierarchyBuilder::Filter(Snapshot, "gameplaytag");
	Require(TagFiltered.Rows.size() == 2 && TagFiltered.Rows[1].PersistentID == ChildAID,
			"scene hierarchy filter did not search stable object tags");

	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	std::future<hierarchy::SceneHierarchySnapshot> Future =
		hierarchy::SceneHierarchyBuilder::BuildAsync(Scheduler, Document.GetScene(), Document.GetRevision());
	const hierarchy::SceneHierarchySnapshot AsyncSnapshot = Future.get();
	Require(AsyncSnapshot.Rows.size() == Snapshot.Rows.size() && AsyncSnapshot.SceneRevision == Snapshot.SceneRevision,
			"background scene hierarchy build did not reproduce the immutable hierarchy snapshot");
}

void ValidateSceneObjectCommands()
{
	document::SceneDocument Document("SceneObjectCommandValidation");
	const world::ObjectHandle Root = Document.CreateObject("Root");
	const util::UUID RootID = GetPersistentID(Document.GetScene(), Root);
	const world::ObjectHandle Child = Document.CreateObject("Child", Root);
	const util::UUID ChildID = GetPersistentID(Document.GetScene(), Child);
	const auto Camera = Document.GetScene().AddComponent<components::CObjectCameraComponent>(Root);
	const auto Light = Document.GetScene().AddComponent<components::CObjectPointLightComponent>(Child);
	const auto Behaviors = Document.GetScene().AddComponent<components::CObjectBehaviorComponent>(Child);
	const auto RootTransform = Document.GetScene().GetComponent<components::CObjectTransformComponent>(Root);
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(RootTransform).SetPosition({5.0f, 0.0f, 0.0f});
		Access.Resolve(Camera).SetVerticalFieldOfViewDegrees(82.0f);
		Access.Resolve(Light).SetLuminousPowerLumens(9'000.0f);
		components::BehaviorInstance ArchivedBehavior{.Type = 77, .TypeName = "ArchivedBehavior", .SchemaVersion = 3};
		ArchivedBehavior.Properties.emplace("Enabled", true);
		ArchivedBehavior.Properties.emplace("Target", ChildID);
		(void)Access.Resolve(Behaviors).AddBehavior(std::move(ArchivedBehavior));
	}

	Document.Execute(std::make_unique<commands::AddComponentCommand>(Document, ChildID, components::CObjectSpotLightComponent::TypeID));
	Require(Document.GetScene().GetComponent<components::CObjectSpotLightComponent>(Child).IsValid(),
			"add-component command did not attach the requested component");
	Document.Undo();
	Require(!Document.GetScene().GetComponent<components::CObjectSpotLightComponent>(Child).IsValid(),
			"add-component command undo did not remove the attached component");
	Document.Redo();
	Document.Undo();

	Document.Execute(std::make_unique<commands::RemoveComponentCommand>(Document, ChildID, components::CObjectPointLightComponent::TypeID));
	Require(!Document.GetScene().GetComponent<components::CObjectPointLightComponent>(Child).IsValid(),
			"remove-component command did not detach the requested component");
	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectPointLightComponent>(Child)).GetLuminousPowerLumens() == 9'000.0f,
				"remove-component command undo did not restore authored component state");
	}

	Document.Execute(std::make_unique<commands::RenameObjectCommand>(Document, ChildID, "Renamed Child"));
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(Child)).GetName() == "Renamed Child",
				"rename-object command did not apply the requested name");
	}
	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(Child)).GetName() == "Child",
				"rename-object command undo did not restore the original name");
	}
	Document.Redo();

	const world::ObjectHandle Sibling = Document.CreateObject("Sibling", Root);
	const util::UUID SiblingID = GetPersistentID(Document.GetScene(), Sibling);
	Document.GetScene().SetParent(Child, Root, 10);
	Document.GetScene().SetParent(Sibling, Root, 20);
	Document.Execute(std::make_unique<commands::ReparentObjectCommand>(Document, ChildID, RootID, 1));
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(Sibling)).GetSiblingOrder() == 0 &&
					Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(Child)).GetSiblingOrder() == 1,
				"hierarchy reorder did not normalize every affected sibling around the insertion point");
	}
	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(Child)).GetSiblingOrder() == 10 &&
					Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(Sibling)).GetSiblingOrder() == 20,
				"hierarchy reorder undo did not restore every affected sibling order");
	}
	Document.DestroyObject(SiblingID);

	glm::vec3 WorldPositionBefore{0.0f};
	{
		auto Access = Document.GetScene().Read();
		WorldPositionBefore = world::SceneTransformSnapshot::Build(Access).GetPosition(Child);
	}
	Document.Execute(std::make_unique<commands::ReparentObjectCommand>(Document, ChildID, util::UUID{}));
	{
		auto Access = Document.GetScene().Read();
		Require(!Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(Child)).GetParent().IsValid(),
				"reparent-object command did not move the child to the scene root");
		Require(glm::length(world::SceneTransformSnapshot::Build(Access).GetPosition(Child) - WorldPositionBefore) < 1.0e-5f,
				"reparent-object command did not preserve the child world transform");
	}
	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(Child)).GetParent() == Root,
				"reparent-object command undo did not restore the original parent");
	}
	{
		auto Access = Document.GetScene().Write();
		components::CObjectIdentityComponent &Identity = Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(Child));
		Identity.SetTags({"Gameplay", "Interactable"});
		Identity.SetMobility(components::ObjectMobility::Stationary);
		Identity.SetEnabled(false);
		Identity.SetLocked(true);
	}

	Document.GetSelection().SelectOnly(RootID);
	const commands::SceneObjectSnapshot Clipboard = commands::SceneObjectSnapshot::Capture(Document, Document.GetSelection().GetOrdered());
	Require(Clipboard.GetObjectCount() == 2, "scene-object clipboard did not capture the complete selected subtree");
	auto Paste = std::make_unique<commands::PasteObjectsCommand>(Document, Clipboard);
	const std::vector<util::UUID> PastedIDs = Paste->GetCreatedObjects();
	Document.Execute(std::move(Paste));
	Require(PastedIDs.size() == 2 && Document.GetScene().GetObjectCount() == 4,
			"paste-object command did not create a new complete subtree");
	{
		const world::ObjectHandle PastedRoot = Document.GetScene().FindObject(PastedIDs[0]);
		const world::ObjectHandle PastedChild = Document.GetScene().FindObject(PastedIDs[1]);
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(PastedChild)).GetParent() == PastedRoot &&
					Access.Resolve(Access.GetComponent<components::CObjectCameraComponent>(PastedRoot)).GetVerticalFieldOfViewDegrees() ==
						82.0f &&
					Access.Resolve(Access.GetComponent<components::CObjectPointLightComponent>(PastedChild)).GetLuminousPowerLumens() ==
						9'000.0f,
				"scene-object clipboard paste did not preserve components or remap internal hierarchy identities");
	}
	Document.Undo();
	Require(Document.GetScene().GetObjectCount() == 2, "paste-object command undo did not remove the pasted subtree");
	Document.Redo();
	Require(Document.GetScene().GetObjectCount() == 4 && Document.GetScene().FindObject(PastedIDs[0]).IsValid(),
			"paste-object command redo did not restore its stable generated identities");
	Document.Undo();

	auto Duplicate = std::make_unique<commands::DuplicateObjectsCommand>(Document, Document.GetSelection().GetOrdered());
	const std::vector<util::UUID> DuplicateIDs = Duplicate->GetCreatedObjects();
	Document.Execute(std::move(Duplicate));
	Require(DuplicateIDs.size() == 2 && Document.GetScene().GetObjectCount() == 4 &&
				Document.GetScene().FindObject(DuplicateIDs[0]).IsValid() && Document.GetScene().FindObject(DuplicateIDs[1]).IsValid(),
			"duplicate-object command did not recreate the complete selected subtree with new identities");
	{
		const world::ObjectHandle DuplicateRoot = Document.GetScene().FindObject(DuplicateIDs[0]);
		const world::ObjectHandle DuplicateChild = Document.GetScene().FindObject(DuplicateIDs[1]);
		auto Access = Document.GetScene().Read();
		const components::CObjectIdentityComponent &DuplicateIdentity =
			Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(DuplicateChild));
		const components::BehaviorInstance &OriginalBehavior =
			Access.Resolve(Access.GetComponent<components::CObjectBehaviorComponent>(Child)).GetBehaviors().front();
		const components::BehaviorInstance &DuplicateBehavior =
			Access.Resolve(Access.GetComponent<components::CObjectBehaviorComponent>(DuplicateChild)).GetBehaviors().front();
		Require(Access.Resolve(Access.GetComponent<components::CObjectCameraComponent>(DuplicateRoot)).GetVerticalFieldOfViewDegrees() ==
						82.0f &&
					Access.Resolve(Access.GetComponent<components::CObjectPointLightComponent>(DuplicateChild)).GetLuminousPowerLumens() ==
						9'000.0f &&
					Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(DuplicateChild)).GetParent() ==
						DuplicateRoot &&
					Access.Resolve(Access.GetComponent<components::CObjectBehaviorComponent>(DuplicateChild)).GetBehaviors().size() == 1 &&
					DuplicateBehavior.InstanceID != OriginalBehavior.InstanceID &&
					std::get<util::UUID>(DuplicateBehavior.Properties.at("Target")) == DuplicateIDs[1] &&
					DuplicateIdentity.GetTags().size() == 2 && DuplicateIdentity.GetMobility() == components::ObjectMobility::Stationary &&
					!DuplicateIdentity.IsEnabled() && DuplicateIdentity.IsLocked(),
				"duplicate-object command did not preserve components or remap the copied hierarchy");
	}
	Document.Undo();
	Require(Document.GetScene().GetObjectCount() == 2, "duplicate-object command undo did not remove the copied subtree");
	Document.Redo();

	Document.Execute(std::make_unique<commands::DeleteObjectsCommand>(Document, DuplicateIDs));
	Require(Document.GetScene().GetObjectCount() == 2, "delete-object command did not remove the selected subtree");
	Document.Undo();
	Require(Document.GetScene().GetObjectCount() == 4 && Document.GetScene().FindObject(DuplicateIDs[0]).IsValid() &&
				Document.GetScene().FindObject(DuplicateIDs[1]).IsValid(),
			"delete-object command undo did not restore the archived subtree identities");

	document::SceneDocument StaleParentDocument("StaleParentCommandValidation");
	const world::ObjectHandle StaleParent = StaleParentDocument.CreateObject("Temporary Parent");
	const util::UUID StaleParentID = GetPersistentID(StaleParentDocument.GetScene(), StaleParent);
	auto CreateWithParent = std::make_unique<commands::CreateObjectCommand>(StaleParentDocument, "Must Not Re-root", StaleParentID);
	const util::UUID CreatedID = CreateWithParent->GetPersistentID();
	StaleParentDocument.DestroyObject(StaleParentID);
	bool MissingParentRejected = false;
	try
	{
		StaleParentDocument.Execute(std::move(CreateWithParent));
	}
	catch (const std::out_of_range &)
	{
		MissingParentRejected = true;
	}
	Require(MissingParentRejected && !StaleParentDocument.GetScene().FindObject(CreatedID).IsValid(),
			"create-object redo silently re-rooted an object after its parent disappeared");

	class MergeFailureCommand final : public commands::EditorCommand
	{
	  public:
		explicit MergeFailureCommand(int32 &Value) : Value(&Value)
		{
		}
		[[nodiscard]] string_view GetName() const noexcept override
		{
			return "Merge Failure Validation";
		}
		void Execute() override
		{
			++*this->Value;
		}
		void Undo() override
		{
			--*this->Value;
		}
		[[nodiscard]] bool TryMerge(const commands::EditorCommand &) override
		{
			throw std::runtime_error("intentional merge failure");
		}

	  private:
		int32 *Value = nullptr;
	};
	commands::CommandHistory History(8);
	int32 ExecutedValue = 0;
	History.Execute(std::make_unique<MergeFailureCommand>(ExecutedValue));
	bool MergeFailureObserved = false;
	try
	{
		History.Execute(std::make_unique<MergeFailureCommand>(ExecutedValue));
	}
	catch (const std::runtime_error &)
	{
		MergeFailureObserved = true;
	}
	Require(MergeFailureObserved && ExecutedValue == 1 && History.GetUndoCount() == 1,
			"command-history publication failure did not roll the newly executed command back");

	document::SceneDocument DirectHistoryDocument("DirectHistoryRevisionValidation");
	const world::ObjectHandle DirectObject = DirectHistoryDocument.CreateObject("Before");
	const util::UUID DirectObjectID = GetPersistentID(DirectHistoryDocument.GetScene(), DirectObject);
	const uint64 BeforeDirectExecute = DirectHistoryDocument.GetRevision();
	DirectHistoryDocument.GetHistory().Execute(
		std::make_unique<commands::RenameObjectCommand>(DirectHistoryDocument, DirectObjectID, "After"));
	Require(DirectHistoryDocument.GetRevision() == BeforeDirectExecute + 1U,
			"direct command-history execution bypassed the owning document revision");
	const uint64 BeforeDirectUndo = DirectHistoryDocument.GetRevision();
	DirectHistoryDocument.GetHistory().Undo();
	Require(DirectHistoryDocument.GetRevision() == BeforeDirectUndo + 1U,
			"direct command-history undo bypassed the owning document revision");
	DirectHistoryDocument.GetHistory().Redo();
	DirectHistoryDocument.SetName("Externally modified document");
	bool RevisionConflictRejected = false;
	try
	{
		DirectHistoryDocument.GetHistory().Undo();
	}
	catch (const std::logic_error &)
	{
		RevisionConflictRejected = true;
	}
	{
		const auto Identity = DirectHistoryDocument.GetScene().GetComponent<components::CObjectIdentityComponent>(DirectObject);
		auto Access = DirectHistoryDocument.GetScene().Read();
		Require(RevisionConflictRejected && DirectHistoryDocument.GetHistory().GetUndoCount() == 1 &&
					Access.Resolve(Identity).GetName() == "After",
				"command history accepted a stale undo after the owning document revision changed");
	}

	document::SceneDocument RawSceneMutationDocument("RawSceneMutationRevisionValidation");
	const world::ObjectHandle RawSceneObject = RawSceneMutationDocument.CreateObject("Before");
	const util::UUID RawSceneObjectID = GetPersistentID(RawSceneMutationDocument.GetScene(), RawSceneObject);
	RawSceneMutationDocument.GetHistory().Execute(
		std::make_unique<commands::RenameObjectCommand>(RawSceneMutationDocument, RawSceneObjectID, "CommandValue"));
	RawSceneMutationDocument.MarkSaved("RawSceneMutation.enginelevel");
	const uint64 BeforeRawMutation = RawSceneMutationDocument.GetRevision();
	{
		const auto Identity = RawSceneMutationDocument.GetScene().GetComponent<components::CObjectIdentityComponent>(RawSceneObject);
		auto Access = RawSceneMutationDocument.GetScene().Write();
		Access.Resolve(Identity).SetName("ExternalValue");
	}
	bool RawSceneRevisionConflictRejected = false;
	try
	{
		RawSceneMutationDocument.GetHistory().Undo();
	}
	catch (const std::logic_error &)
	{
		RawSceneRevisionConflictRejected = true;
	}
	Require(RawSceneMutationDocument.GetRevision() > BeforeRawMutation, "raw mutable scene access did not advance the document revision");
	Require(RawSceneMutationDocument.IsDirty(), "raw mutable scene access did not dirty the document");
	Require(RawSceneRevisionConflictRejected, "command history accepted an undo after raw mutable scene access");
	Require(RawSceneMutationDocument.GetHistory().GetUndoCount() == 1, "stale raw-scene undo rejection changed the command-history stacks");
}

void ValidateSceneCloning()
{
	document::SceneDocument Document("CloneValidation");
	const world::ObjectHandle Root = Document.CreateObject("Root");
	const world::ObjectHandle Child = Document.CreateObject("Child", Root);
	const util::UUID RootID = GetPersistentID(Document.GetScene(), Root);
	const util::UUID ChildID = GetPersistentID(Document.GetScene(), Child);
	const world::ComponentHandle<components::CObjectCameraComponent> Camera =
		Document.GetScene().AddComponent<components::CObjectCameraComponent>(Root);
	const world::ComponentHandle<components::CObjectPointLightComponent> Light =
		Document.GetScene().AddComponent<components::CObjectPointLightComponent>(Child);
	const world::ComponentHandle<components::CObjectBehaviorComponent> Behaviors =
		Document.GetScene().AddComponent<components::CObjectBehaviorComponent>(Child);
	const auto RootIdentity = Document.GetScene().GetComponent<components::CObjectIdentityComponent>(Root);
	{
		auto Access = Document.GetScene().Write();
		Access.Resolve(RootIdentity).SetTags({"Clone", "Root"});
		Access.Resolve(RootIdentity).SetMobility(components::ObjectMobility::Static);
		components::CObjectCameraComponent &CameraComponent = Access.Resolve(Camera);
		CameraComponent.SetVerticalFieldOfViewDegrees(75.0f);
		CameraComponent.SetPrimary(true);
		components::CObjectPointLightComponent &LightComponent = Access.Resolve(Light);
		LightComponent.SetLuminousPowerLumens(4'500.0f);
		LightComponent.SetRange(42.0f);
		components::BehaviorInstance Behavior{.Type = 123, .TypeName = "CloneBehavior", .SchemaVersion = 7};
		Behavior.Properties.emplace("Speed", float32{12.5f});
		components::CObjectBehaviorComponent &BehaviorComponent = Access.Resolve(Behaviors);
		const components::BehaviorHandle BehaviorHandle = BehaviorComponent.AddBehavior(std::move(Behavior));
		BehaviorComponent.SetExecutionState(BehaviorHandle.InstanceID, components::BehaviorExecutionState::Constructed);
		BehaviorComponent.SetExecutionState(BehaviorHandle.InstanceID, components::BehaviorExecutionState::Active);
	}

	world::SceneCloneResult Clone = world::SceneCloner::Clone(Document.GetScene());
	Require(Clone.ClonedScene != nullptr && Clone.ClonedScene->GetObjectCount() == Document.GetScene().GetObjectCount(),
			"scene clone did not preserve the complete object set");
	const world::ObjectHandle ClonedRoot = Clone.FindObject(RootID);
	const world::ObjectHandle ClonedChild = Clone.FindObject(ChildID);
	Require(ClonedRoot.IsValid() && ClonedChild.IsValid() && ClonedRoot.Scene != Root.Scene && ClonedChild.Scene != Child.Scene,
			"scene clone did not create isolated runtime object handles");
	{
		auto Access = Clone.ClonedScene->Read();
		const auto ClonedHierarchy = Access.GetComponent<components::CObjectHierarchyComponent>(ClonedChild);
		const auto ClonedIdentity = Access.GetComponent<components::CObjectIdentityComponent>(ClonedRoot);
		const auto ClonedCamera = Access.GetComponent<components::CObjectCameraComponent>(ClonedRoot);
		const auto ClonedLight = Access.GetComponent<components::CObjectPointLightComponent>(ClonedChild);
		const auto ClonedBehaviors = Access.GetComponent<components::CObjectBehaviorComponent>(ClonedChild);
		Require(Access.Resolve(ClonedHierarchy).GetParent() == ClonedRoot,
				"scene clone did not remap hierarchy handles into the cloned scene");
		Require(Access.Resolve(ClonedIdentity).GetTags().size() == 2 &&
					Access.Resolve(ClonedIdentity).GetMobility() == components::ObjectMobility::Static,
				"scene clone did not preserve identity tags and mobility");
		Require(Access.Resolve(ClonedCamera).GetVerticalFieldOfViewDegrees() == 75.0f && Access.Resolve(ClonedCamera).IsPrimary(),
				"scene clone did not preserve authored camera properties");
		Require(Access.Resolve(ClonedLight).GetLuminousPowerLumens() == 4'500.0f && Access.Resolve(ClonedLight).GetRange() == 42.0f,
				"scene clone did not preserve authored light properties");
		const std::vector<components::BehaviorInstance> &ClonedInstances = Access.Resolve(ClonedBehaviors).GetBehaviors();
		Require(ClonedInstances.size() == 1 && ClonedInstances.front().Type == 123 && ClonedInstances.front().SchemaVersion == 7 &&
					ClonedInstances.front().Properties.contains("Speed") &&
					ClonedInstances.front().State == components::BehaviorExecutionState::Unresolved &&
					ClonedInstances.front().Diagnostic.empty(),
				"scene clone did not preserve authored behavior data while resetting runtime-only state");
	}
}

void ValidatePlaySession()
{
	ValidationConstructs.store(0, std::memory_order_relaxed);
	ValidationStarts.store(0, std::memory_order_relaxed);
	ValidationUpdates.store(0, std::memory_order_relaxed);
	ValidationFixedUpdates.store(0, std::memory_order_relaxed);
	ValidationStops.store(0, std::memory_order_relaxed);
	ValidationDestroys.store(0, std::memory_order_relaxed);

	document::SceneDocument Document("PlaySessionValidation");
	const world::ObjectHandle EditObject = Document.CreateObject("RuntimeObject");
	const util::UUID PersistentID = GetPersistentID(Document.GetScene(), EditObject);
	const world::ComponentHandle<components::CObjectBehaviorComponent> BehaviorComponent =
		Document.GetScene().AddComponent<components::CObjectBehaviorComponent>(EditObject);
	{
		auto Access = Document.GetScene().Write();
		components::CObjectBehaviorComponent &Component = Access.Resolve(BehaviorComponent);
		components::BehaviorInstance ValidationBehavior{.Type = 700, .TypeName = "ValidationBehavior", .SchemaVersion = 1};
		ValidationBehavior.Properties.emplace("Speed", float32{2.0f});
		(void)Component.AddBehavior(std::move(ValidationBehavior));
		components::BehaviorInstance ThrowingBehavior{.Type = 701, .TypeName = "ThrowingBehavior", .SchemaVersion = 1};
		ThrowingBehavior.Properties.emplace("Speed", float32{2.0f});
		(void)Component.AddBehavior(std::move(ThrowingBehavior));
	}

	runtime::behavior::BehaviorRegistry Registry;
	Registry.Register(MakeValidationBehaviorDescriptor(700, "ValidationBehavior", false));
	Registry.Register(MakeValidationBehaviorDescriptor(701, "ThrowingBehavior", true));
	const uint64 StableRegistryGeneration = Registry.GetGeneration();
	bool DuplicateRegistrationRejected = false;
	try
	{
		Registry.Register(MakeValidationBehaviorDescriptor(702, "ValidationBehavior", false));
	}
	catch (const runtime::behavior::DuplicateBehaviorDescriptorException &)
	{
		DuplicateRegistrationRejected = true;
	}
	Require(DuplicateRegistrationRejected && !Registry.Contains(702) && Registry.GetGeneration() == StableRegistryGeneration,
			"behavior registry duplicate publication was not strongly transactional");
	std::atomic<uint32> FixedSimulationSteps = 0;
	std::atomic<uint32> VariableSimulationSteps = 0;
	resource::AssetManager Assets(std::filesystem::current_path());
	play::PlaySession Session(
		Assets, Registry,
		{.FixedDeltaSeconds = 1.0 / 60.0,
		 .MaximumFrameDeltaSeconds = 0.25,
		 .MaximumFixedStepsPerFrame = 4,
		 .FixedSimulation = [&FixedSimulationSteps](world::Scene &, core::threading::TaskScheduler &, const float64)
		 { FixedSimulationSteps.fetch_add(1, std::memory_order_relaxed); },
		 .VariableSimulation = [&VariableSimulationSteps](world::Scene &, core::threading::TaskScheduler &, const float64)
		 { VariableSimulationSteps.fetch_add(1, std::memory_order_relaxed); }});
	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	Session.Start(Document.GetScene());
	Require(Session.GetState() == play::PlaySessionState::Playing && Session.HasRuntimeScene(),
			"play session did not enter the playing state with an isolated runtime scene");
	const world::ObjectHandle RuntimeObject = Session.FindRuntimeObject(PersistentID);
	Require(RuntimeObject.IsValid() && RuntimeObject.Scene != EditObject.Scene,
			"play session did not map the persistent edit object into an isolated runtime object");
	Require(ValidationConstructs.load(std::memory_order_relaxed) == 2 && ValidationStarts.load(std::memory_order_relaxed) == 2,
			"play-session startup did not construct and start every enabled behavior exactly once");

	Session.Tick(Scheduler, 1.0 / 30.0);
	const play::PlaySessionStatistics FirstTick = Session.GetStatistics();
	Require(ValidationUpdates.load(std::memory_order_relaxed) == 1 && ValidationFixedUpdates.load(std::memory_order_relaxed) == 4,
			"parallel behavior execution did not run the successful update and two fixed updates for both instances");
	Require(FirstTick.Behaviors.ActiveInstances == 1 && FirstTick.Behaviors.FailedInstances == 1,
			"one failing behavior was not isolated while the other behavior remained active");
	const std::vector<runtime::behavior::BehaviorStateSnapshot> CapturedState = Session.SuspendBehaviorsForReload();
	Require(Session.GetState() == play::PlaySessionState::Reloading,
			"behavior suspension did not place the play session in its explicit reload-quiescent state");
	const auto CapturedActive =
		std::ranges::find(CapturedState, components::BehaviorTypeID{700}, &runtime::behavior::BehaviorStateSnapshot::Type);
	ValidationBehaviorState CapturedBehaviorState;
	if (CapturedActive != CapturedState.end() && CapturedActive->Data.size() == sizeof(ValidationBehaviorState))
		std::memcpy(&CapturedBehaviorState, CapturedActive->Data.data(), sizeof(CapturedBehaviorState));
	Require(CapturedState.size() == 2 && CapturedActive != CapturedState.end() && CapturedActive->HasRuntimeState &&
				CapturedActive->Data.size() == sizeof(ValidationBehaviorState) && CapturedBehaviorState.UpdateCount == 1,
			"behavior hot-reload capture did not preserve the active instance state");
	runtime::behavior::BehaviorDescriptor MigratedBehavior = MakeValidationBehaviorDescriptor(700, "ValidationBehavior", false);
	runtime::behavior::BehaviorDescriptor MigratedThrowing = MakeValidationBehaviorDescriptor(701, "ThrowingBehavior", true);
	MigratedBehavior.SchemaVersion = 2;
	MigratedBehavior.MigrateProperties = &MigrateValidationProperties;
	MigratedBehavior.Properties.push_back({.Name = "Migrated", .DefaultValue = false});
	MigratedThrowing.SchemaVersion = 2;
	MigratedThrowing.MigrateProperties = &MigrateValidationProperties;
	MigratedThrowing.Properties.push_back({.Name = "Migrated", .DefaultValue = false});
	const std::array MigratedDescriptors{MigratedBehavior, MigratedThrowing};
	Registry.ReplaceAll(MigratedDescriptors);
	Session.RestoreBehaviorsAfterReload(CapturedState);
	{
		const auto RuntimeBehavior = Session.GetRuntimeScene()->GetComponent<components::CObjectBehaviorComponent>(RuntimeObject);
		const auto Access = Session.GetRuntimeScene()->Read();
		const std::optional<components::BehaviorInstance> Migrated =
			Access.Resolve(RuntimeBehavior).FindBehavior(CapturedActive->InstanceID);
		Require(Migrated.has_value() && Migrated->SchemaVersion == 2 && Migrated->Properties.contains("Migrated") &&
					std::get<bool>(Migrated->Properties.at("Migrated")),
				"behavior hot reload did not migrate authored properties and schema before state restoration");
	}
	{
		const world::ComponentHandle<components::CObjectTransformComponent> RuntimeTransform =
			Session.GetRuntimeScene()->GetComponent<components::CObjectTransformComponent>(RuntimeObject);
		const world::ComponentHandle<components::CObjectTransformComponent> EditTransform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(EditObject);
		const auto RuntimeAccess = Session.GetRuntimeScene()->Read();
		const auto EditAccess = Document.GetScene().Read();
		Require(RuntimeAccess.Resolve(RuntimeTransform).GetPosition().x > 0.0f &&
					EditAccess.Resolve(EditTransform).GetPosition() == glm::vec3(0.0f),
				"runtime behavior mutation leaked from the cloned play world into the authored edit scene");
	}

	Session.Pause();
	const play::PlaySessionApplyBackResult ApplyBackResult = Session.ApplyBack(Document);
	Require(ApplyBackResult.ChangedObjects == 1, "play-session apply-back did not identify the changed runtime transform");
	{
		const auto RuntimeAccess = Session.GetRuntimeScene()->Read();
		const auto EditAccess = Document.GetScene().Read();
		const auto RuntimeTransform = RuntimeAccess.GetComponent<components::CObjectTransformComponent>(RuntimeObject);
		const auto EditTransform = EditAccess.GetComponent<components::CObjectTransformComponent>(EditObject);
		Require(EditAccess.Resolve(EditTransform).GetPosition() == RuntimeAccess.Resolve(RuntimeTransform).GetPosition(),
				"play-session apply-back did not publish the runtime transform into the authoring scene");
	}
	Document.Undo();
	{
		const auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectTransformComponent>(EditObject)).GetPosition() == glm::vec3(0.0f),
				"play-session apply-back did not produce one undoable authoring transaction");
	}
	const uint32 UpdatesBeforePause = ValidationUpdates.load(std::memory_order_relaxed);
	Session.Tick(Scheduler, 0.1);
	Require(ValidationUpdates.load(std::memory_order_relaxed) == UpdatesBeforePause,
			"paused play session continued running variable behavior updates");
	Session.Step(Scheduler);
	Require(ValidationUpdates.load(std::memory_order_relaxed) == UpdatesBeforePause + 1,
			"paused single-step did not execute exactly one variable behavior update");
	Session.Stop();
	Require(Session.GetState() == play::PlaySessionState::Stopped && !Session.HasRuntimeScene(),
			"stopping play did not discard the isolated runtime scene");
	Require(ValidationStops.load(std::memory_order_relaxed) == 4 && ValidationDestroys.load(std::memory_order_relaxed) == 4,
			"play-session teardown did not stop and destroy every constructed behavior exactly once");
	{
		const auto Access = Document.GetScene().Read();
		const std::vector<components::BehaviorInstance> &Authored = Access.Resolve(BehaviorComponent).GetBehaviors();
		Require(Authored.size() == 2 && Authored[0].State == components::BehaviorExecutionState::Unresolved &&
					Authored[1].State == components::BehaviorExecutionState::Unresolved,
				"play-session lifecycle state leaked back into the authored behavior components");
	}

	const uint32 ConstructsBeforeSimulate = ValidationConstructs.load(std::memory_order_relaxed);
	const uint32 StartsBeforeSimulate = ValidationStarts.load(std::memory_order_relaxed);
	const uint32 UpdatesBeforeSimulate = ValidationUpdates.load(std::memory_order_relaxed);
	FixedSimulationSteps.store(0, std::memory_order_relaxed);
	VariableSimulationSteps.store(0, std::memory_order_relaxed);
	Session.Start(Document.GetScene(), play::PlaySessionMode::Simulate);
	Require(Session.GetMode() == play::PlaySessionMode::Simulate && Session.HasRuntimeScene(),
			"simulate did not create an isolated editor-controlled scene");
	Session.Tick(Scheduler, 1.0 / 30.0);
	Session.Stop();
	Require(ValidationConstructs.load(std::memory_order_relaxed) == ConstructsBeforeSimulate &&
				ValidationStarts.load(std::memory_order_relaxed) == StartsBeforeSimulate &&
				ValidationUpdates.load(std::memory_order_relaxed) == UpdatesBeforeSimulate,
			"simulate invoked game behavior lifecycle callbacks");
	Require(FixedSimulationSteps.load(std::memory_order_relaxed) == 2 && VariableSimulationSteps.load(std::memory_order_relaxed) == 1,
			"simulate did not execute the configured fixed and variable world systems");
}

void ValidateAssetRegistry()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLAssetRegistryValidation-" + util::UUID::GenerateRandomUUID().ToString());
	std::error_code Error;
	std::filesystem::create_directories(Root / "Textures", Error);
	if (Error)
		throw std::runtime_error("Could not create asset-registry validation directories: " + Error.message());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code CleanupError;
			std::filesystem::remove_all(this->Root, CleanupError);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	const auto WriteFile = [](const std::filesystem::path &Path, const string_view Contents)
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		if (!Stream)
			throw std::runtime_error("Could not create asset-registry validation file");
		Stream.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
		if (!Stream)
			throw std::runtime_error("Could not write asset-registry validation file");
	};
	WriteFile(Root / "Textures" / "Brick.PNG", "texture");
	WriteFile(Root / "World.scene", "{}");
	WriteFile(Root / "Readme.xyz", "unknown");

	asset::AssetRegistry Registry(Root);
	Registry.StartWatching();
	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	Registry.RequestRefresh(Scheduler, true);
	Registry.WaitForRefresh();
	const asset::AssetRegistrySnapshot &Initial = Registry.GetSnapshot();
	string InitialScanDiagnostic = "initial content scan published " + std::to_string(Initial.Entries.size()) + " entries";
	for (const string &Diagnostic : Initial.Diagnostics)
		InitialScanDiagnostic += "; " + Diagnostic;
	Require(Initial.Revision == 1 && Initial.Entries.size() == 4 && Initial.Diagnostics.empty(), InitialScanDiagnostic);
	const auto FindPath = [](const asset::AssetRegistrySnapshot &Snapshot, const string_view RelativePath)
	{
		const auto Entry = std::ranges::find_if(Snapshot.Entries, [RelativePath](const asset::ContentEntry &Candidate)
												{ return Candidate.RelativePath.generic_string() == RelativePath; });
		return Entry == Snapshot.Entries.end() ? static_cast<const asset::ContentEntry *>(nullptr) : &*Entry;
	};
	const asset::ContentEntry *Texture = FindPath(Initial, "Textures/Brick.PNG");
	const asset::ContentEntry *Scene = FindPath(Initial, "World.scene");
	Require(Texture != nullptr && Texture->Kind == asset::ContentEntryKind::Asset && Texture->AssetType == resource::AssetType::Texture2D &&
				Scene != nullptr && Scene->Kind == asset::ContentEntryKind::Scene,
			"content registry did not classify case-insensitive texture and scene extensions");
	Require(Initial.Search("brick").size() == 1 && Initial.Search("TEXTURES").size() == 2,
			"content-registry search is not case-insensitive across relative paths");
	const resource::AssetID TextureIdentity = Texture->ID;
	const string TexturePhysicalIdentity = Texture->PhysicalSourceIdentity;
	resource::AssetManager PublishedAssets(Root);
	Registry.PublishTo(PublishedAssets);
	try
	{
		(void)PublishedAssets.GetAssetByID<resource::Texture2DAsset>(TextureIdentity);
	}
	catch (const resource::importer::AssetImportException &)
	{
	}
	std::optional<resource::AssetRecordSnapshot> PublishedTexture = PublishedAssets.SnapshotRecord(TextureIdentity);
	const std::optional<resource::AssetPublication> VirtualTexture = PublishedAssets.ResolveVirtualPath("/Game/Textures/Brick.PNG");
	Require(PublishedTexture.has_value() && PublishedTexture->ID == TextureIdentity && VirtualTexture.has_value() &&
				VirtualTexture->ID == TextureIdentity,
			"ID-first asset loading did not reserve the authoritative sidecar UUID");

	WriteFile(Root / "Notes.txt", "change notification");
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < Deadline && !Registry.HasUnappliedChanges())
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	Require(Registry.HasUnappliedChanges(), "content watcher did not observe a file creation before the validation deadline");
	Registry.RequestRefresh(Scheduler);
	Registry.WaitForRefresh();
	Require(FindPath(Registry.GetSnapshot(), "Notes.txt") != nullptr,
			"recursive content change notification did not trigger a background registry refresh");

	std::filesystem::rename(Root / "Textures" / "Brick.PNG", Root / "Textures" / "Renamed.png", Error);
	if (Error)
		throw std::runtime_error("Could not rename asset-registry validation file: " + Error.message());
	Registry.RequestRefresh(Scheduler, true);
	Registry.WaitForRefresh();
	const asset::ContentEntry *Renamed = FindPath(Registry.GetSnapshot(), "Textures/Renamed.png");
	string RegistryDiagnostics;
	for (const string &Diagnostic : Registry.GetSnapshot().Diagnostics)
		RegistryDiagnostics += " [" + Diagnostic + "]";
	Require(Renamed != nullptr && Renamed->ID == TextureIdentity,
			"content registry did not preserve the authoritative metadata identity across a rename (expected '" + TextureIdentity +
				"', observed '" + (Renamed == nullptr ? string("<missing>") : Renamed->ID) + "', physical identity changed from '" +
				TexturePhysicalIdentity + "' to '" + (Renamed == nullptr ? string("<missing>") : Renamed->PhysicalSourceIdentity) +
				"'). Diagnostics:" + RegistryDiagnostics);
	Registry.PublishTo(PublishedAssets);
	PublishedTexture = PublishedAssets.SnapshotRecord(TextureIdentity);
	const std::optional<resource::AssetPublication> RenamedVirtualTexture =
		PublishedAssets.ResolveVirtualPath("/Game/Textures/Renamed.png");
	Require(PublishedTexture.has_value() && PublishedTexture->CanonicalPath == Root / "Textures" / "Renamed.png" &&
				!PublishedAssets.ResolveVirtualPath("/Game/Textures/Brick.PNG").has_value() && RenamedVirtualTexture.has_value() &&
				RenamedVirtualTexture->ID == TextureIdentity,
			"asset registry publication did not update the stable record path after rename");
	Registry.StopWatching();
}

void ValidateAssetImport()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLAssetImportValidation-" + util::UUID::GenerateRandomUUID().ToString());
	const std::filesystem::path Content = Root / "Content";
	const std::filesystem::path Intermediate = Root / "Intermediate";
	const std::filesystem::path FirstSource = Root / "FirstSource";
	const std::filesystem::path SecondSource = Root / "SecondSource";
	std::error_code Error;
	std::filesystem::create_directories(Content, Error);
	std::filesystem::create_directories(Intermediate, Error);
	std::filesystem::create_directories(FirstSource, Error);
	std::filesystem::create_directories(SecondSource, Error);
	if (Error)
		throw std::runtime_error("Could not create asset-import validation directories: " + Error.message());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code CleanupError;
			std::filesystem::remove_all(this->Root, CleanupError);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	const auto WriteFile = [](const std::filesystem::path &Path, const string_view Contents)
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
		if (!Stream)
			throw std::runtime_error("Could not write asset-import validation fixture");
	};
	const auto ReadFile = [](const std::filesystem::path &Path)
	{
		std::ifstream Stream(Path, std::ios::binary);
		return string(std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>());
	};
	WriteFile(FirstSource / "Same.glsl", "first");
	WriteFile(SecondSource / "Same.glsl", "second");
	WriteFile(FirstSource / "Third.glsl", "third");

	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	asset::AssetRegistry Registry(Content);
	Registry.RequestRefresh(Scheduler, true);
	Registry.WaitForRefresh();
	asset::AssetImportService Import(Content, Intermediate);
	const auto WaitForImport = [&]()
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (Import.IsBusy() && std::chrono::steady_clock::now() < Deadline)
		{
			(void)Import.Poll(Scheduler, Registry);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		(void)Import.Poll(Scheduler, Registry);
		Require(!Import.IsBusy(), "asset import did not complete before the validation deadline");
	};

	Import.Queue(
		{.Sources = {FirstSource / "Same.glsl", SecondSource / "Same.glsl"}, .CollisionPolicy = asset::ImportCollisionPolicy::KeepBoth},
		Scheduler);
	WaitForImport();
	Require(Import.GetResult().has_value() && Import.GetResult()->Committed && Import.GetResult()->GetImportedCount() == 2 &&
				ReadFile(Content / "Same.glsl") == "first" && ReadFile(Content / "Same-1.glsl") == "second" &&
				Import.GetProgress().CompletedFiles == 2 && Import.GetProgress().GetFraction() == 1.0f,
			"keep-both asset import did not commit unique deterministic destination names");
	string MetadataDiagnostic;
	const std::optional<asset::AssetMetadata> InitialMetadata =
		asset::AssetMetadataStore::TryLoad(Content / "Same.glsl.assetmeta", MetadataDiagnostic);
	Require(InitialMetadata.has_value(), "asset import did not transactionally publish authoritative metadata: " + MetadataDiagnostic);
	Registry.WaitForRefresh();
	Require(Registry.GetSnapshot().Search("same").size() == 2,
			"successful asset import did not force a refreshed content-registry snapshot");

	Import.Reset();
	WriteFile(SecondSource / "Same.glsl", "replacement");
	Import.Queue({.Sources = {SecondSource / "Same.glsl"}, .CollisionPolicy = asset::ImportCollisionPolicy::Replace}, Scheduler);
	WaitForImport();
	const std::optional<asset::AssetMetadata> ReplacedMetadata =
		asset::AssetMetadataStore::TryLoad(Content / "Same.glsl.assetmeta", MetadataDiagnostic);
	Require(Import.GetResult()->Committed && ReadFile(Content / "Same.glsl") == "replacement" && ReplacedMetadata.has_value() &&
				ReplacedMetadata->ID == InitialMetadata->ID && ReplacedMetadata->SourceHash != InitialMetadata->SourceHash,
			"explicit replace import did not atomically replace the existing content file");

	Import.Reset();
	Import.Queue({.Sources = {FirstSource / "Third.glsl", Root / "Missing.glsl"}, .CollisionPolicy = asset::ImportCollisionPolicy::Fail},
				 Scheduler);
	WaitForImport();
	Require(Import.GetResult().has_value() && !Import.GetResult()->Committed && Import.GetResult()->GetFailedCount() != 0 &&
				!std::filesystem::exists(Content / "Third.glsl") && !std::filesystem::exists(Content / "Third.glsl.assetmeta"),
			"failed import validation left a partially published file in project Content");

	Import.Reset();
	WriteFile(FirstSource / "Cancel.bin", string(8U * 1'024U * 1'024U, 'x'));
	Import.Queue({.Sources = {FirstSource / "Cancel.bin"}}, Scheduler);
	Import.Cancel();
	WaitForImport();
	Require(Import.GetState() == asset::AssetImportServiceState::Cancelled && !std::filesystem::exists(Content / "Cancel.bin"),
			"cancelled asset import published a partial destination");
}

void ValidateAssetContentOperations()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLAssetContentValidation-" + util::UUID::GenerateRandomUUID().ToString());
	const std::filesystem::path Content = Root / "Content";
	const std::filesystem::path Intermediate = Root / "Intermediate";
	const std::filesystem::path Trash = Root / "Saved" / "Trash";
	std::filesystem::create_directories(Content / "Source");
	std::filesystem::create_directories(Intermediate);
	std::filesystem::create_directories(Trash);
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	const auto WriteFile = [](const std::filesystem::path &Path, const string_view Contents)
	{
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
		if (!Stream)
			throw std::runtime_error("Could not write content-operation validation fixture");
	};
	const auto CreateMetadata = [](const std::filesystem::path &Path, const string_view VirtualPath)
	{
		asset::AssetMetadata Metadata = asset::AssetMetadataStore::Create(Path, string(VirtualPath), "ShaderSource");
		asset::AssetMetadataStore::Save(Metadata, asset::AssetMetadataStore::GetSidecarPath(Path));
		return Metadata;
	};

	const std::filesystem::path OriginalPath = Content / "Source" / "Original.glsl";
	WriteFile(OriginalPath, "void main() {}\n");
	const asset::AssetMetadata OriginalMetadata = CreateMetadata(OriginalPath, "/Game/Source/Original.glsl");
	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 64});
	asset::AssetRegistry Registry(Content);
	asset::AssetContentService Service(Content, Intermediate, Trash);
	const auto Execute = [&](asset::AssetContentRequest Request)
	{
		Service.Queue(std::move(Request), Scheduler);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (Service.IsBusy() && std::chrono::steady_clock::now() < Deadline)
		{
			(void)Service.Poll(Scheduler, Registry);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		(void)Service.Poll(Scheduler, Registry);
		Require(!Service.IsBusy() && Service.GetResult().has_value(),
				"asset content operation did not complete before the validation deadline");
		return *Service.GetResult();
	};

	asset::AssetContentResult Result = Execute({.Operation = asset::AssetContentOperation::CreateFolder, .Destination = "Authored"});
	Require(Result.Committed && std::filesystem::is_directory(Content / "Authored"),
			"content directory creation did not publish the requested directory");

	Service.Reset();
	Result = Execute({.Operation = asset::AssetContentOperation::CreateMaterial, .Destination = "Authored/Surface.material"});
	const material::MaterialDocument BaseMaterial = material::MaterialDocumentStore::Load(Content / "Authored" / "Surface.material");
	Require(Result.Committed && BaseMaterial.Type == material::MaterialDocumentType::Material && BaseMaterial.Name == "Surface",
			"base-material creation did not publish a valid native material document");

	const resource::AssetID ParentID = util::UUID::GenerateRandomUUID().ToString();
	Service.Reset();
	Result = Execute({.Operation = asset::AssetContentOperation::CreateMaterialInstance,
					  .Destination = "Authored/Surface Instance.materialinstance",
					  .ParentAssetID = ParentID,
					  .ParentAssetType = resource::AssetType::Material});
	const material::MaterialDocument MaterialInstance =
		material::MaterialDocumentStore::Load(Content / "Authored" / "Surface Instance.materialinstance");
	Require(Result.Committed && MaterialInstance.Type == material::MaterialDocumentType::MaterialInstance &&
				MaterialInstance.Parent.has_value() && MaterialInstance.Parent->ID == ParentID &&
				MaterialInstance.Parent->Type == resource::AssetType::Material,
			"material-instance creation did not preserve its typed stable parent reference");

	Service.Reset();
	Result =
		Execute({.Operation = asset::AssetContentOperation::Move, .Source = "Source/Original.glsl", .Destination = "Moved/Renamed.glsl"});
	Require(Result.Committed && !std::filesystem::exists(OriginalPath) &&
				std::filesystem::is_regular_file(Content / "Moved" / "Renamed.glsl"),
			"content move did not atomically relocate the source");
	string Diagnostic;
	const std::optional<asset::AssetMetadata> MovedMetadata =
		asset::AssetMetadataStore::TryLoad(Content / "Moved" / "Renamed.glsl.assetmeta", Diagnostic);
	Require(MovedMetadata.has_value() && MovedMetadata->ID == OriginalMetadata.ID &&
				MovedMetadata->VirtualSource == "/Game/Moved/Renamed.glsl",
			"content move changed stable identity or failed to update its canonical virtual path");

	Service.Reset();
	Result = Execute(
		{.Operation = asset::AssetContentOperation::Duplicate, .Source = "Moved/Renamed.glsl", .Destination = "Moved/Renamed Copy.glsl"});
	const std::optional<asset::AssetMetadata> DuplicateMetadata =
		asset::AssetMetadataStore::TryLoad(Content / "Moved" / "Renamed Copy.glsl.assetmeta", Diagnostic);
	Require(Result.Committed && DuplicateMetadata.has_value() && DuplicateMetadata->ID != OriginalMetadata.ID &&
				DuplicateMetadata->VirtualSource == "/Game/Moved/Renamed Copy.glsl",
			"content duplication did not assign a new stable identity and canonical path");

	Service.Reset();
	Result = Execute({.Operation = asset::AssetContentOperation::Trash, .Source = "Moved/Renamed Copy.glsl"});
	const std::vector<asset::TrashedContentEntry> Trashed = Service.ScanTrash();
	Require(Result.Committed && !std::filesystem::exists(Content / "Moved" / "Renamed Copy.glsl") && Trashed.size() == 1 &&
				Trashed.front().OriginalPath == std::filesystem::path("Moved/Renamed Copy.glsl"),
			"soft delete did not preserve a restorable project-trash record");

	Service.Reset();
	Result = Execute({.Operation = asset::AssetContentOperation::Restore, .TrashEntryID = Trashed.front().ID});
	const std::optional<asset::AssetMetadata> RestoredMetadata =
		asset::AssetMetadataStore::TryLoad(Content / "Moved" / "Renamed Copy.glsl.assetmeta", Diagnostic);
	Require(Result.Committed && RestoredMetadata.has_value() && RestoredMetadata->ID == DuplicateMetadata->ID &&
				Service.ScanTrash().empty(),
			"content restore did not preserve the trashed stable identity or consume its manifest");

	std::filesystem::create_directories(Content / "Bundle");
	WriteFile(Content / "Bundle" / "Dependency.glsl", "dependency\n");
	WriteFile(Content / "Bundle" / "Dependent.glsl", "dependent\n");
	asset::AssetMetadata Dependency = CreateMetadata(Content / "Bundle" / "Dependency.glsl", "/Game/Bundle/Dependency.glsl");
	asset::AssetMetadata Dependent = CreateMetadata(Content / "Bundle" / "Dependent.glsl", "/Game/Bundle/Dependent.glsl");
	Dependent.Dependencies.push_back(Dependency.ID);
	asset::AssetMetadataStore::Save(Dependent, Content / "Bundle" / "Dependent.glsl.assetmeta");
	Service.Reset();
	Result = Execute({.Operation = asset::AssetContentOperation::Duplicate, .Source = "Bundle", .Destination = "Bundle Copy"});
	const std::optional<asset::AssetMetadata> CopiedDependency =
		asset::AssetMetadataStore::TryLoad(Content / "Bundle Copy" / "Dependency.glsl.assetmeta", Diagnostic);
	const std::optional<asset::AssetMetadata> CopiedDependent =
		asset::AssetMetadataStore::TryLoad(Content / "Bundle Copy" / "Dependent.glsl.assetmeta", Diagnostic);
	Require(Result.Committed && CopiedDependency.has_value() && CopiedDependent.has_value() && CopiedDependency->ID != Dependency.ID &&
				CopiedDependent->ID != Dependent.ID && CopiedDependent->Dependencies.size() == 1 &&
				CopiedDependent->Dependencies.front() == CopiedDependency->ID,
			"directory duplication did not remap internal asset dependencies to copied stable identities");

	Service.Reset();
	Result = Execute(
		{.Operation = asset::AssetContentOperation::Move, .Source = "Moved/Renamed.glsl", .Destination = "Moved/Renamed Copy.glsl"});
	Require(!Result.Committed && Service.GetState() == asset::AssetContentServiceState::Failed &&
				std::filesystem::is_regular_file(Content / "Moved" / "Renamed.glsl"),
			"content collision failure mutated or removed the original source");
}

void ValidateMaterialAssets()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLMaterialValidation-" + util::UUID::GenerateRandomUUID().ToString());
	const std::filesystem::path Content = Root / "Content";
	std::filesystem::create_directories(Content);
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	const std::array<uint8, 22> Targa{0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
									  0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x28, 0x20, 0x40, 0x80, 0xFF};
	const std::filesystem::path TexturePath = Content / "Surface.tga";
	{
		std::ofstream Stream(TexturePath, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char *>(Targa.data()), static_cast<std::streamsize>(Targa.size()));
	}
	asset::AssetMetadata TextureMetadata = asset::AssetMetadataStore::Create(TexturePath, "/Game/Surface.tga", "Texture2D");
	asset::AssetMetadataStore::Save(TextureMetadata, asset::AssetMetadataStore::GetSidecarPath(TexturePath));
	const std::filesystem::path MaterialPath = Content / "Surface.material";
	{
		std::ofstream Stream(MaterialPath, std::ios::binary | std::ios::trunc);
		Stream << R"({"FormatVersion":1,"AssetType":"Material","Name":"Surface","Pipeline":{"ShadingModel":"ClearCoat",)"
				  R"("BlendMode":"Opaque","TwoSided":true},"Factors":{"BaseColor":[0.8,0.6,0.4,1.0],"Metallic":0.7,)"
				  R"("Roughness":0.25,"ClearCoat":0.9},"Textures":[{"Semantic":"BaseColor","AssetID":")"
			   << TextureMetadata.ID << R"(","TextureCoordinateChannel":0}]})";
	}

	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	asset::AssetRegistry Registry(Content);
	Registry.RequestRefresh(Scheduler, true);
	Registry.WaitForRefresh();
	resource::AssetManager Assets(Content);
	Registry.PublishTo(Assets);
	const resource::AssetHandle<resource::MaterialAsset> Material = Assets.GetAsset<resource::MaterialAsset>(MaterialPath);
	const resource::AssetPtr<resource::MaterialAsset> MaterialAsset = Material.Pin();
	Require(MaterialAsset->GetPipelineContract().ShadingModel == resource::MaterialShadingModel::ClearCoat &&
				MaterialAsset->GetPipelineContract().TwoSided && std::abs(MaterialAsset->GetFactors().Metallic - 0.7f) < 1.0e-5f &&
				MaterialAsset->GetTextures().size() == 1 && MaterialAsset->GetTextures().front().Texture.GetID() == TextureMetadata.ID &&
				MaterialAsset->GetTextures().front().Texture.Pin()->GetWidth() == 1,
			"native PBR material import did not resolve its complete pipeline, factors, and stable texture handle");

	const asset::ContentEntry *MaterialEntry = Registry.GetSnapshot().FindByVirtualPath("/Game/Surface.material");
	Require(MaterialEntry != nullptr, "material registry entry was not published");
	const resource::AssetID MaterialID = MaterialEntry->ID;
	{
		std::ofstream Stream(MaterialPath, std::ios::binary | std::ios::trunc);
		Stream << R"({"FormatVersion":1,"AssetType":"Material","Name":"Surface","Pipeline":{"ShadingModel":"ClearCoat",)"
				  R"("BlendMode":"Opaque","TwoSided":true},"Factors":{"BaseColor":[0.8,0.6,0.4,1.0],"Metallic":0.2,)"
				  R"("Roughness":0.25,"ClearCoat":0.9},"Textures":[{"Semantic":"BaseColor","AssetID":")"
			   << TextureMetadata.ID << R"(","TextureCoordinateChannel":0}]})";
	}
	asset::AssetReloadService ReloadService(Assets, Content);
	ReloadService.Begin(Scheduler, MaterialID, MaterialEntry->MetadataPath);
	ReloadService.Wait();
	Require(ReloadService.GetResult().has_value() && ReloadService.GetResult()->Succeeded &&
				std::abs(Material.Pin()->GetFactors().Metallic - 0.2f) < 1.0e-5f,
			"asynchronous ID-first material reload did not publish the replacement generation");
	string ReloadedMetadataDiagnostic;
	const std::optional<asset::AssetMetadata> ReloadedMetadata =
		asset::AssetMetadataStore::TryLoad(Content / MaterialEntry->MetadataPath, ReloadedMetadataDiagnostic);
	Require(ReloadedMetadata.has_value() && ReloadedMetadata->Dependencies.size() == 1 &&
				ReloadedMetadata->Dependencies.front() == TextureMetadata.ID,
			"asset reload did not publish discovered dependencies back to the authoritative metadata sidecar");
	const std::filesystem::path InstancePath = Content / "Surface Variant.materialinstance";
	{
		std::ofstream Stream(InstancePath, std::ios::binary | std::ios::trunc);
		Stream << R"({"FormatVersion":1,"AssetType":"MaterialInstance","Name":"Surface Variant","Parent":{"AssetType":"Material",)"
				  R"("AssetID":")"
			   << MaterialID
			   << R"("},"Pipeline":{"ShadingModel":"DefaultLit","BlendMode":"Masked"},"Factors":{"BaseColor":[0.2,0.3,0.4,1.0],)"
				  R"("Metallic":0.1,"Roughness":0.8,"AlphaCutoff":0.35},"Textures":[]})";
	}
	Registry.RequestRefresh(Scheduler, true);
	Registry.WaitForRefresh();
	Registry.PublishTo(Assets);
	const resource::AssetHandle<resource::MaterialInstanceAsset> Instance = Assets.GetAsset<resource::MaterialInstanceAsset>(InstancePath);
	const resource::AssetPtr<resource::MaterialInstanceAsset> InstanceAsset = Instance.Pin();
	Require(InstanceAsset->GetParent().GetID() == MaterialID &&
				InstanceAsset->GetPipelineContract().BlendMode == resource::MaterialBlendMode::Masked &&
				std::abs(InstanceAsset->GetFactors().AlphaCutoff - 0.35f) < 1.0e-5f,
			"material instance import did not preserve its parent handle and resolved overrides");
	material::MaterialDocument AuthoringDocument = material::MaterialDocumentStore::Load(MaterialPath);
	AuthoringDocument.Factors.Roughness = 0.63f;
	AuthoringDocument.Pipeline.ReceivesShadows = false;
	material::MaterialDocumentStore::Save(AuthoringDocument);
	const material::MaterialDocument ReloadedAuthoringDocument = material::MaterialDocumentStore::Load(MaterialPath);
	Require(std::abs(ReloadedAuthoringDocument.Factors.Roughness - 0.63f) < 1.0e-5f &&
				!ReloadedAuthoringDocument.Pipeline.ReceivesShadows && ReloadedAuthoringDocument.Textures.front().ID == TextureMetadata.ID,
			"material authoring document did not atomically round-trip complete editable state");
	material::MaterialEditorSession EditorSession = material::MaterialEditorSession::Open(MaterialPath, Assets);
	material::MaterialDocument BeforeColorGesture = EditorSession.GetDocument();
	const glm::vec4 EditedBaseColor(0.17f, 0.46f, 0.79f, 1.0f);
	EditorSession.Edit().Factors.BaseColor = EditedBaseColor;
	EditorSession.BeginEditGesture(std::move(BeforeColorGesture));
	Require(EditorSession.HasActiveEditGesture() && glm::all(glm::equal(EditorSession.GetDocument().Factors.BaseColor, EditedBaseColor)),
			"material editor base-color gesture did not retain its live document value while active");
	EditorSession.EndEditGesture();
	Require(!EditorSession.HasActiveEditGesture() && EditorSession.IsDirty() &&
				glm::all(glm::equal(EditorSession.GetDocument().Factors.BaseColor, EditedBaseColor)),
			"closing the material editor base-color gesture restored the previous document color");
	EditorSession.Save();
	Require(!EditorSession.IsDirty() &&
				glm::all(glm::equal(material::MaterialDocumentStore::Load(MaterialPath).Factors.BaseColor, EditedBaseColor)),
			"material editor save did not persist the base color selected when the picker closed");
	const asset::ContentEntry *const ReloadEntry = Registry.GetSnapshot().Find(MaterialID);
	Require(ReloadEntry != nullptr, "material editor base-color reload lost its registry identity");
	asset::AssetReloadService ColorReloadService(Assets, Content);
	ColorReloadService.Begin(Scheduler, MaterialID, ReloadEntry->MetadataPath);
	ColorReloadService.Wait();
	Require(ColorReloadService.GetResult().has_value() && ColorReloadService.GetResult()->Succeeded &&
				glm::all(glm::equal(Material.Pin()->GetFactors().BaseColor, EditedBaseColor)),
			"material asset reload restored the base color that preceded the saved editor gesture");
	EditorSession.Reload(true);
	Require(glm::all(glm::equal(EditorSession.GetDocument().Factors.BaseColor, EditedBaseColor)),
			"material editor reload did not retain its saved base color");

	const std::filesystem::path InvalidPath = Content / "Invalid.material";
	{
		std::ofstream Stream(InvalidPath, std::ios::binary | std::ios::trunc);
		Stream << R"({"FormatVersion":1,"AssetType":"Material","Name":"Invalid","Factors":{"Metallic":4.0}})";
	}
	bool TypedFailure = false;
	try
	{
		(void)Assets.GetAsset<resource::MaterialAsset>(InvalidPath);
	}
	catch (const resource::importer::AssetMaterialParseException &)
	{
		TypedFailure = true;
	}
	Require(TypedFailure, "invalid material content did not throw its typed material parse exception");
}

void ValidateModelImporterSecureDependencies()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLModelDependencyValidation-" + util::UUID::GenerateRandomUUID().ToString());
	const std::filesystem::path Content = Root / "Content";
	const std::filesystem::path Models = Content / "Models";
	const std::filesystem::path Textures = Content / "Textures";
	const std::filesystem::path Outside = Root / "Outside";
	const std::filesystem::path Junction = Models / "Link";
	const std::filesystem::path ShaderJunction = Content / "ShaderLink";
	std::filesystem::create_directories(Models);
	std::filesystem::create_directories(Textures);
	std::filesystem::create_directories(Content / "Shaders");
	std::filesystem::create_directories(Content / "Shared");
	std::filesystem::create_directories(Outside);
	struct Cleanup final
	{
		std::filesystem::path Junction;
		std::filesystem::path ShaderJunction;
		std::filesystem::path Root;
		~Cleanup()
		{
			(void)RemoveDirectoryW(this->Junction.c_str());
			(void)RemoveDirectoryW(this->ShaderJunction.c_str());
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Junction, ShaderJunction, Root};
	const std::array<uint8, 22> Targa{0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
									  0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x28, 0x20, 0x40, 0x80, 0xFF};
	const auto WriteTarga = [&Targa](const std::filesystem::path &Path)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char *>(Targa.data()), static_cast<std::streamsize>(Targa.size()));
		if (!Output)
			throw std::runtime_error("Could not write model dependency texture fixture");
	};
	const auto WriteModel = [](const std::filesystem::path &Path, const string_view MaterialLibrary)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output << "mtllib " << MaterialLibrary
			   << "\n"
				  "o SecureDependencyTriangle\n"
				  "v 0 0 0\n"
				  "v 1 0 0\n"
				  "v 0 1 0\n"
				  "vt 0 0\n"
				  "vt 1 0\n"
				  "vt 0 1\n"
				  "vn 0 0 1\n"
				  "usemtl Surface\n"
				  "f 1/1/1 2/2/1 3/3/1\n";
		if (!Output)
			throw std::runtime_error("Could not write model dependency geometry fixture");
	};
	WriteTarga(Textures / "Surface.tga");
	WriteTarga(Outside / "Escape.tga");
	{
		std::ofstream Material(Models / "Valid.mtl", std::ios::binary | std::ios::trunc);
		Material << "newmtl Surface\nmap_Kd ../Textures/Surface.tga\n";
	}
	WriteModel(Models / "Valid.obj", "Valid.mtl");
	resource::AssetManager Assets(Content);
	const resource::AssetHandle<resource::ModelAsset> Valid = Assets.GetAsset<resource::ModelAsset>("Models/Valid.obj");
	Require(Valid.Pin() != nullptr, "model importer rejected a contained sibling-directory texture dependency");

	CreateValidationJunction(Junction, Outside);
	{
		std::ofstream Material(Models / "Escaping.mtl", std::ios::binary | std::ios::trunc);
		Material << "newmtl Surface\nmap_Kd Link/Escape.tga\n";
	}
	WriteModel(Models / "Escaping.obj", "Escaping.mtl");
	bool JunctionRejected = false;
	try
	{
		(void)Assets.GetAsset<resource::ModelAsset>("Models/Escaping.obj");
	}
	catch (const resource::importer::AssetContentValidationException &)
	{
		JunctionRejected = true;
	}
	Require(JunctionRejected, "model importer followed an external-texture junction outside the trusted asset root");

	{
		std::ofstream Common(Content / "Shared" / "Common.glsl", std::ios::binary | std::ios::trunc);
		Common << "vec4 ValidationColor() { return vec4(1.0); }\n";
		std::ofstream Shader(Content / "Shaders" / "Contained.vert", std::ios::binary | std::ios::trunc);
		Shader << "#version 460 core\n#include \"../Shared/Common.glsl\"\nvoid main() { gl_Position = ValidationColor(); }\n";
	}
	const resource::AssetHandle<pipeline::shader::ShaderSourceAsset> ContainedShader =
		Assets.GetAsset<pipeline::shader::ShaderSourceAsset>(resource::AssetType::ShaderSource, "Shaders/Contained.vert");
	Require(ContainedShader.Pin() != nullptr, "shader importer rejected a contained sibling-directory include dependency");
	CreateValidationJunction(ShaderJunction, Outside);
	{
		std::ofstream Escape(Outside / "Escape.glsl", std::ios::binary | std::ios::trunc);
		Escape << "vec4 EscapedColor() { return vec4(0.0); }\n";
		std::ofstream Shader(Content / "Shaders" / "Escaping.vert", std::ios::binary | std::ios::trunc);
		Shader << "#version 460 core\n#include \"../ShaderLink/Escape.glsl\"\nvoid main() { gl_Position = EscapedColor(); }\n";
	}
	bool ShaderJunctionRejected = false;
	try
	{
		(void)Assets.GetAsset<pipeline::shader::ShaderSourceAsset>(resource::AssetType::ShaderSource, "Shaders/Escaping.vert");
	}
	catch (const resource::importer::AssetContentValidationException &)
	{
		ShaderJunctionRejected = true;
	}
	Require(ShaderJunctionRejected, "shader importer followed an include junction outside the trusted asset root");
}

void ValidatePrimitiveAssets()
{
	resource::AssetManager Assets;
	asset::PrimitiveMeshFactory Factory(Assets);
	std::unordered_set<resource::AssetID> ModelIDs;
	std::unordered_set<resource::AssetID> MeshIDs;
	std::unordered_set<string> DerivedDataKeys;
	for (usize Index = 0; Index < static_cast<usize>(asset::PrimitiveShape::Count); ++Index)
	{
		const asset::PrimitiveShape Shape = static_cast<asset::PrimitiveShape>(Index);
		const resource::AssetHandle<resource::ModelAsset> ModelHandle = Factory.GetModel(Shape);
		const resource::AssetHandle<resource::ModelAsset> CachedModelHandle = Factory.GetModel(Shape);
		Require(ModelHandle && CachedModelHandle.GetID() == ModelHandle.GetID() && ModelIDs.insert(ModelHandle.GetID()).second,
				"primitive factory did not cache a unique deterministic model identity");
		const resource::AssetPtr<resource::ModelAsset> Model = ModelHandle.Pin();
		Require(Model->GetName() == asset::PrimitiveMeshFactory::GetName(Shape) && Model->GetBounds().IsValid() &&
					Model->GetNodes().size() == 1 && Model->GetMeshInstances().size() == 1,
				"primitive model does not expose its complete node, bounds, and mesh-instance foundation");
		const resource::AssetHandle<resource::MeshAsset> &MeshHandle = Model->GetMeshInstances().front().Mesh;
		Require(MeshHandle && MeshIDs.insert(MeshHandle.GetID()).second, "primitive model does not own a unique mesh asset identity");
		const resource::AssetPtr<resource::MeshAsset> Mesh = MeshHandle.Pin();
		const usize ExpectedLODCount = Shape == asset::PrimitiveShape::Box || Shape == asset::PrimitiveShape::Plane ? 1U : 4U;
		Require(Mesh->GetKind() == resource::MeshKind::Static && Mesh->GetBounds().IsValid() &&
					Mesh->GetCPURetentionPolicy() == resource::MeshCPURetentionPolicy::RetainAll && Mesh->GetMaterialSlots().size() == 1 &&
					Mesh->GetMaterialSlots().front().DefaultMaterial && Mesh->GetLODs().size() == ExpectedLODCount &&
					!Mesh->GetDerivedDataKey().empty() && DerivedDataKeys.insert(string(Mesh->GetDerivedDataKey())).second,
				"primitive mesh does not expose deterministic LOD, material, retention, bounds, or derived-data contracts");
		for (const resource::MeshLOD &LOD : Mesh->GetLODs())
		{
			std::unordered_set<resource::MeshVertexSemantic> Semantics;
			for (const resource::MeshVertexStream &Stream : LOD.VertexStreams)
				Semantics.insert(Stream.Semantic);
			Require(LOD.Bounds.IsValid() && LOD.CPUGeometryResident && LOD.IndexStream.IndexCount > 0 && LOD.Sections.size() == 1 &&
						LOD.Sections.front().IndexCount == LOD.IndexStream.IndexCount &&
						LOD.Sections.front().MaterialSlot == Mesh->GetMaterialSlots().front().ID &&
						Semantics.contains(resource::MeshVertexSemantic::Position) &&
						Semantics.contains(resource::MeshVertexSemantic::Normal) &&
						Semantics.contains(resource::MeshVertexSemantic::Tangent) &&
						Semantics.contains(resource::MeshVertexSemantic::TextureCoordinate),
					"primitive LOD does not contain complete indexed PBR vertex streams and section metadata");

			const auto PositionStream = std::ranges::find_if(LOD.VertexStreams, [](const resource::MeshVertexStream &Stream)
															 { return Stream.Semantic == resource::MeshVertexSemantic::Position; });
			const auto NormalStream = std::ranges::find_if(LOD.VertexStreams, [](const resource::MeshVertexStream &Stream)
														   { return Stream.Semantic == resource::MeshVertexSemantic::Normal; });
			Require(PositionStream != LOD.VertexStreams.end() && NormalStream != LOD.VertexStreams.end() &&
						PositionStream->Format == resource::MeshVertexFormat::Float32x3 &&
						NormalStream->Format == resource::MeshVertexFormat::Float32x3 && PositionStream->Stride >= sizeof(glm::vec3) &&
						NormalStream->Stride >= sizeof(glm::vec3) && (LOD.IndexStream.IndexCount % 3U) == 0U,
					"primitive LOD cannot be validated as an indexed triangle list");
			const auto ReadIndex = [&LOD](const uint32 Offset)
			{
				uint32 Value = 0;
				if (LOD.IndexStream.Format == resource::MeshIndexFormat::UInt16)
				{
					uint16 CompactValue = 0;
					std::memcpy(&CompactValue, LOD.IndexStream.Bytes.data() + static_cast<usize>(Offset) * sizeof(uint16), sizeof(uint16));
					Value = CompactValue;
				}
				else
				{
					std::memcpy(&Value, LOD.IndexStream.Bytes.data() + static_cast<usize>(Offset) * sizeof(uint32), sizeof(uint32));
				}
				return Value;
			};
			const auto ReadVector = [](const resource::MeshVertexStream &Stream, const uint32 Vertex)
			{
				glm::vec3 Value{0.0f};
				std::memcpy(&Value, Stream.Bytes.data() + static_cast<usize>(Vertex) * Stream.Stride, sizeof(Value));
				return Value;
			};
			for (uint32 IndexOffset = 0; IndexOffset < LOD.IndexStream.IndexCount; IndexOffset += 3U)
			{
				const std::array Triangle{ReadIndex(IndexOffset), ReadIndex(IndexOffset + 1U), ReadIndex(IndexOffset + 2U)};
				Require(
					std::ranges::all_of(Triangle, [&PositionStream](const uint32 Vertex) { return Vertex < PositionStream->ElementCount; }),
					"primitive LOD contains an out-of-range triangle index");
				const glm::vec3 PositionA = ReadVector(*PositionStream, Triangle[0]);
				const glm::vec3 PositionB = ReadVector(*PositionStream, Triangle[1]);
				const glm::vec3 PositionC = ReadVector(*PositionStream, Triangle[2]);
				const glm::vec3 FaceNormal = glm::cross(PositionB - PositionA, PositionC - PositionA);
				if (glm::dot(FaceNormal, FaceNormal) <= 1.0e-12f)
					continue;
				const glm::vec3 DeclaredNormal = ReadVector(*NormalStream, Triangle[0]) + ReadVector(*NormalStream, Triangle[1]) +
												 ReadVector(*NormalStream, Triangle[2]);
				Require(glm::dot(FaceNormal, DeclaredNormal) > 0.0f,
						"primitive LOD triangle winding disagrees with its declared outward normals");
			}
		}
	}
	document::SceneDocument Document("PrimitiveCommandProvenanceValidation");
	bool MismatchedPrimitiveRejected = false;
	try
	{
		(void)commands::CreatePrimitiveCommand(Document, asset::PrimitiveShape::Box, Factory.GetModel(asset::PrimitiveShape::Sphere));
	}
	catch (const std::invalid_argument &)
	{
		MismatchedPrimitiveRejected = true;
	}
	Require(MismatchedPrimitiveRejected, "create-primitive command accepted a model from a different primitive shape");
}

void ValidatePrivateMaterialAssignments()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLPrivateMaterialValidation-" + util::UUID::GenerateRandomUUID().ToString());
	const std::filesystem::path Content = Root / "Content";
	const std::filesystem::path Intermediate = Root / "Intermediate";
	std::filesystem::create_directories(Content);
	std::filesystem::create_directories(Intermediate);
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};

	resource::AssetManager Assets(Content);
	asset::PrimitiveMeshFactory Factory(Assets);
	document::SceneDocument Document("PrivateMaterialValidation");
	const world::ObjectHandle Object = Document.CreateObject("Cylinder");
	const resource::AssetHandle<resource::ModelAsset> Model = Factory.GetModel(asset::PrimitiveShape::Cylinder);
	(void)Document.GetScene().AddComponent<components::CObjectMeshComponent>(Object, Model);
	const auto PinnedModel = Model.Pin();
	const resource::ModelMeshInstance &Instance = PinnedModel->GetMeshInstances().front();
	const resource::MaterialSlotID Slot = Instance.Mesh.Pin()->GetMaterialSlots().front().ID;
	const material::PrivateMaterialTarget Target{.Object = Object, .MeshInstance = Instance.ID, .MaterialSlot = Slot};
	{
		const resource::AssetHandle<resource::MaterialInterfaceAsset> Parent =
			Instance.Mesh.Pin()->GetMaterialSlots().front().DefaultMaterial;
		const resource::AssetPtr<resource::MaterialInterfaceAsset> PinnedParent = Parent.Pin();
		const resource::AssetID ReplacementID = util::UUID::GenerateRandomUUID().ToString();
		const std::filesystem::path ReplacementPath = Content / "PublishedReplacement.materialinstance";
		const std::array Publication{resource::AssetPublication{.ID = ReplacementID,
																.CanonicalPath = ReplacementPath,
																.VirtualPath = "/Game/PublishedReplacement.materialinstance",
																.Type = resource::AssetType::MaterialInstance,
																.Dependencies = {Parent.GetID()}}};
		Assets.PublishAssetRegistry(Publication);
		resource::GeneratedAssetStage Replacement = Assets.StageGeneratedAsset<resource::MaterialInstanceAsset>(
			ReplacementID, ReplacementPath,
			resource::AssetPtr<resource::MaterialInstanceAsset>::Make(
				"Published Replacement", Parent, PinnedParent->GetPipelineContract(), PinnedParent->GetFactors(),
				std::vector<resource::MaterialTextureBinding>(PinnedParent->GetTextures().begin(), PinnedParent->GetTextures().end())),
			{Parent.GetID()});
		resource::GeneratedAssetStage *ReplacementPointer = &Replacement;
		Assets.CommitGeneratedAssets(std::span(&ReplacementPointer, 1));
		Require(Replacement.IsCommitted() && Assets.GetRecord(ReplacementID) != nullptr,
				"private material update could not replace an exact published asset identity after project reload");
		Assets.PublishAssetRegistry(Publication);
		resource::PBRMaterialFactors UpdatedFactors = PinnedParent->GetFactors();
		UpdatedFactors.BaseColor = glm::vec4(0.21f, 0.43f, 0.65f, 1.0f);
		resource::GeneratedAssetStage RepeatedReplacement = Assets.StageGeneratedAsset<resource::MaterialInstanceAsset>(
			ReplacementID, ReplacementPath,
			resource::AssetPtr<resource::MaterialInstanceAsset>::Make(
				"Repeated Published Replacement", Parent, PinnedParent->GetPipelineContract(), UpdatedFactors,
				std::vector<resource::MaterialTextureBinding>(PinnedParent->GetTextures().begin(), PinnedParent->GetTextures().end())),
			{Parent.GetID()});
		resource::GeneratedAssetStage *RepeatedReplacementPointer = &RepeatedReplacement;
		Assets.CommitGeneratedAssets(std::span(&RepeatedReplacementPointer, 1));
		Require(RepeatedReplacement.IsCommitted() &&
					glm::all(glm::equal(RepeatedReplacement.GetHandle<resource::MaterialInstanceAsset>().Pin()->GetFactors().BaseColor,
										UpdatedFactors.BaseColor)),
				"repeated private material update rejected an exact identity present in generated and published asset tables");
	}

	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	asset::AssetRegistry Registry(Content);
	material::PrivateMaterialAssignmentService Service(Assets, Content, Intermediate, Root / "Trash");
	const auto WaitForOperation =
		[&Scheduler, &Registry](material::PrivateMaterialAssignmentService &AssignmentService, const util::UUID &OperationID)
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			(void)AssignmentService.Poll(Scheduler, Registry);
			if (std::optional<material::PrivateMaterialAssignmentResult> Result = AssignmentService.TakeResult(OperationID);
				Result.has_value())
			{
				return *std::move(Result);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		throw std::runtime_error("Private material operation did not complete before its deadline: " + OperationID.ToString());
	};
	const glm::vec4 RequestedColor(0.12f, 0.34f, 0.56f, 1.0f);
	const glm::vec4 PreviewColor(0.72f, 0.18f, 0.41f, 1.0f);
	const util::UUID CancelledPreview = Service.BeginBaseColorPreview(Document, std::span(&Target, 1), PreviewColor);
	{
		auto Access = Document.GetScene().Read();
		const components::CObjectMeshComponent &Mesh = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object));
		Require(Mesh.GetMaterialOverrides().size() == 1 &&
					glm::all(glm::equal(Mesh.GetMaterialOverrides().front().Material.Pin()->GetFactors().BaseColor, PreviewColor)),
				"private material color preview did not update the live scene immediately");
		Require(Document.GetHistory().GetUndoCount() == 0, "private material color preview polluted command history before commit");
	}
	Service.UpdateBaseColorPreview(CancelledPreview, RequestedColor);
	{
		auto Access = Document.GetScene().Read();
		const components::CObjectMeshComponent &Mesh = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object));
		Require(glm::all(glm::equal(Mesh.GetMaterialOverrides().front().Material.Pin()->GetFactors().BaseColor, RequestedColor)),
				"private material color preview retained an obsolete picker value");
	}
	Service.CancelBaseColorPreview(CancelledPreview);
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object)).GetMaterialOverrides().empty(),
				"canceling a private material color preview did not restore the original assignment");
		Require(Document.GetHistory().GetUndoCount() == 0, "canceling a private material color preview created an undo command");
	}
	const util::UUID CommittedPreview = Service.BeginBaseColorPreview(Document, std::span(&Target, 1), RequestedColor);
	const util::UUID InitialOperation = Service.CommitBaseColorPreview(CommittedPreview, Scheduler);
	Require(Document.GetHistory().GetUndoCount() == 1,
			"committing one private material color gesture did not produce exactly one undo command");
	bool RejectedConcurrentTarget = false;
	try
	{
		(void)Service.BeginBaseColorAssignment(Document, std::span(&Target, 1), glm::vec4(0.8f, 0.1f, 0.2f, 1.0f), Scheduler);
	}
	catch (const std::logic_error &)
	{
		RejectedConcurrentTarget = true;
	}
	Require(RejectedConcurrentTarget, "private material service allowed two pending writes to the same deterministic target");
	resource::AssetHandle<resource::MaterialInterfaceAsset> PrivateMaterial;
	util::UUID ObjectID;
	{
		auto Access = Document.GetScene().Read();
		const auto Mesh = Access.GetComponent<components::CObjectMeshComponent>(Object);
		const auto Identity = Access.GetComponent<components::CObjectIdentityComponent>(Object);
		const components::CObjectMeshComponent &Component = Access.Resolve(Mesh);
		Require(Component.GetMaterialOverrides().size() == 1, "private material assignment did not publish an immediate mesh override");
		PrivateMaterial = Component.GetMaterialOverrides().front().Material;
		ObjectID = Access.Resolve(Identity).GetPersistentID();
	}
	Require(glm::all(glm::equal(PrivateMaterial.Pin()->GetFactors().BaseColor, RequestedColor)),
			"private material assignment did not publish the requested base color immediately");

	const material::PrivateMaterialAssignmentResult InitialResult = WaitForOperation(Service, InitialOperation);
	const std::filesystem::path MaterialPath =
		Content / "Generated" / "Materials" /
		(ObjectID.ToString() + "_" + std::to_string(Instance.ID) + "_" + std::to_string(Slot) + ".materialinstance");
	const bool MaterialPersisted = std::filesystem::is_regular_file(MaterialPath);
	const bool MetadataPersisted = std::filesystem::is_regular_file(asset::AssetMetadataStore::GetSidecarPath(MaterialPath));
	Require(InitialResult.Committed && MaterialPersisted && MetadataPersisted,
			"private material assignment did not atomically persist its material document and metadata (committed=" +
				std::to_string(InitialResult.Committed) + ", material=" + std::to_string(MaterialPersisted) +
				", metadata=" + std::to_string(MetadataPersisted) + ", diagnostic=" + InitialResult.Diagnostic + ")");
	const material::MaterialDocument Persisted = material::MaterialDocumentStore::Load(MaterialPath);
	Require(glm::all(glm::equal(Persisted.Factors.BaseColor, RequestedColor)) && Persisted.Parent.has_value(),
			"persisted private material instance lost its base color or parent dependency");
	const glm::vec4 RepeatedRequestedColor(0.73f, 0.19f, 0.42f, 1.0f);
	const util::UUID RepeatedPreview = Service.BeginBaseColorPreview(Document, std::span(&Target, 1), RepeatedRequestedColor);
	{
		auto Access = Document.GetScene().Read();
		const components::CObjectMeshComponent &Mesh = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object));
		Require(glm::all(glm::equal(Mesh.GetMaterialOverrides().front().Material.Pin()->GetFactors().BaseColor, RepeatedRequestedColor)),
				"repeated private material color gesture did not preview its selected color immediately");
	}
	const util::UUID RepeatedOperation = Service.CommitBaseColorPreview(RepeatedPreview, Scheduler);
	{
		auto Access = Document.GetScene().Read();
		const components::CObjectMeshComponent &Mesh = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object));
		Require(glm::all(glm::equal(Mesh.GetMaterialOverrides().front().Material.Pin()->GetFactors().BaseColor, RepeatedRequestedColor)),
				"closing a repeated private material color gesture restored the previous color before persistence");
		PrivateMaterial = Mesh.GetMaterialOverrides().front().Material;
	}
	const material::PrivateMaterialAssignmentResult RepeatedResult = WaitForOperation(Service, RepeatedOperation);
	const material::MaterialDocument RepeatedPersisted = material::MaterialDocumentStore::Load(MaterialPath);
	Registry.WaitForRefresh();
	Registry.PublishTo(Assets);
	asset::AssetReloadService ReloadService(Assets, Content);
	ReloadService.BeginChanged(Scheduler);
	ReloadService.Wait();
	const std::optional<asset::AssetReloadResult> &ReloadResult = ReloadService.GetResult();
	{
		auto Access = Document.GetScene().Read();
		const components::CObjectMeshComponent &Mesh = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object));
		Require(
			RepeatedResult.Committed && ReloadResult.has_value() && ReloadResult->Succeeded &&
				glm::all(glm::equal(Mesh.GetMaterialOverrides().front().Material.Pin()->GetFactors().BaseColor, RepeatedRequestedColor)) &&
				glm::all(glm::equal(RepeatedPersisted.Factors.BaseColor, RepeatedRequestedColor)),
			"repeated private material publication or content-reload handoff did not retain the color selected when the picker closed");
	}
	const pipeline::render::SceneRenderSnapshot RenderSnapshot = pipeline::render::SceneRenderSnapshotBuilder::Build(Document.GetScene());
	const auto SnapshotMesh = std::ranges::find(RenderSnapshot.Meshes, Object, &pipeline::render::SceneMeshSnapshot::Owner);
	Require(
		SnapshotMesh != RenderSnapshot.Meshes.end() && SnapshotMesh->MaterialOverrides.size() == 1 &&
			SnapshotMesh->MaterialOverrides.front().MeshInstance == Instance.ID &&
			SnapshotMesh->MaterialOverrides.front().MaterialSlot == Slot &&
			glm::all(glm::equal(SnapshotMesh->MaterialOverrides.front().Material.Pin()->GetFactors().BaseColor, RepeatedRequestedColor)),
		"render snapshot did not retain the committed private material override and selected base color after content reload");

	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		const components::CObjectMeshComponent &Mesh = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object));
		Require(Mesh.GetMaterialOverrides().size() == 1 &&
					glm::all(glm::equal(Mesh.GetMaterialOverrides().front().Material.Pin()->GetFactors().BaseColor, RequestedColor)),
				"undo did not restore the color preceding the repeated private material edit");
	}
	Document.Redo();
	{
		auto Access = Document.GetScene().Read();
		const components::CObjectMeshComponent &Mesh = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object));
		Require(
			Mesh.GetMaterialOverrides().size() == 1 &&
				glm::all(glm::equal(Mesh.GetMaterialOverrides().front().Material.Pin()->GetFactors().BaseColor, RepeatedRequestedColor)),
			"redo did not restore the repeated private material color");
	}
	Document.GetSelection().SelectOnly(ObjectID);
	Document.Execute(std::make_unique<commands::DuplicateObjectsCommand>(Document, Document.GetSelection().GetOrdered()));
	const util::UUID DuplicateObjectID = Document.GetSelection().GetPrimary();
	const world::ObjectHandle DuplicateObject = Document.GetScene().FindObject(DuplicateObjectID);
	const std::array DuplicateObjects{DuplicateObject};
	const std::vector<util::UUID> CloneOperations = Service.ClonePrivateAssignments(Document, DuplicateObjects, Scheduler);
	resource::AssetID DuplicateMaterialID;
	{
		auto Access = Document.GetScene().Read();
		const components::CObjectMeshComponent &DuplicateMesh =
			Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(DuplicateObject));
		Require(CloneOperations.size() == 1 && DuplicateMesh.GetMaterialOverrides().size() == 1,
				"duplicating an object did not schedule one independent private material instance");
		DuplicateMaterialID = DuplicateMesh.GetMaterialOverrides().front().Material.GetID();
	}
	Require(DuplicateMaterialID != PrivateMaterial.GetID(),
			"duplicating an object retained ownership of the source object's private material instance");
	const material::PrivateMaterialAssignmentResult CloneResult = WaitForOperation(Service, CloneOperations.front());
	const std::filesystem::path DuplicateMaterialPath =
		Content / "Generated" / "Materials" /
		(DuplicateObjectID.ToString() + "_" + std::to_string(Instance.ID) + "_" + std::to_string(Slot) + ".materialinstance");
	Require(CloneResult.Committed && std::filesystem::is_regular_file(DuplicateMaterialPath),
			"duplicated private material instance was not persisted under the duplicate object's stable identity");
	Document.Undo();
	Document.Undo();
	Document.Undo();
	Document.Undo();
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object)).GetMaterialOverrides().empty(),
				"private material undo setup did not restore the original empty override state");
	}

	const std::filesystem::path MaterialDirectory = Content / "Generated" / "Materials";
	const std::filesystem::path MaterialDirectoryBackup = Content / "Generated" / "Materials.validation-backup";
	std::filesystem::rename(MaterialDirectory, MaterialDirectoryBackup);
	{
		std::ofstream Blocker(MaterialDirectory, std::ios::binary | std::ios::trunc);
		Blocker << "not a directory";
	}
	material::PrivateMaterialAssignmentService FailingService(Assets, Content, Intermediate, Root / "FailingTrash");
	const util::UUID FailureOperation =
		FailingService.BeginBaseColorAssignment(Document, std::span(&Target, 1), glm::vec4(0.9f), Scheduler);
	const material::PrivateMaterialAssignmentResult FailureResult = WaitForOperation(FailingService, FailureOperation);
	Require(!FailureResult.Committed, "private material persistence failure fixture unexpectedly committed: " + FailureResult.Diagnostic);
	{
		auto Access = Document.GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Object)).GetMaterialOverrides().empty(),
				"failed private material persistence did not roll the live scene assignment back safely");
	}
	std::filesystem::remove(MaterialDirectory);
	std::filesystem::rename(MaterialDirectoryBackup, MaterialDirectory);

	const resource::AssetID ImportedParentID =
		resource::AssetManager::MakeAssetID(resource::AssetType::Material, Content / "Meshes/ValidationCube.obj#material/0");
	const resource::AssetPtr<resource::MaterialInterfaceAsset> DefaultMaterial =
		Instance.Mesh.Pin()->GetMaterialSlots().front().DefaultMaterial.Pin();
	const resource::AssetHandle<resource::MaterialAsset> ImportedParent = Assets.PublishGeneratedAsset<resource::MaterialAsset>(
		ImportedParentID, Content / "Meshes/ValidationCube.obj#material/0",
		resource::AssetPtr<resource::MaterialAsset>::Make(
			"ValidationCube Imported Material", DefaultMaterial->GetPipelineContract(), DefaultMaterial->GetFactors(),
			std::vector<resource::MaterialTextureBinding>(DefaultMaterial->GetTextures().begin(), DefaultMaterial->GetTextures().end())));
	document::SceneDocument ImportedDocument("ImportedPrivateMaterialValidation");
	const world::ObjectHandle ImportedObject = ImportedDocument.CreateObject("ValidationCube");
	(void)ImportedDocument.GetScene().AddComponent<components::CObjectMeshComponent>(ImportedObject, Model);
	{
		auto Access = ImportedDocument.GetScene().Write();
		Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(ImportedObject))
			.SetMaterialOverride(Instance.ID, Slot, ImportedParent);
	}
	const material::PrivateMaterialTarget ImportedTarget{.Object = ImportedObject, .MeshInstance = Instance.ID, .MaterialSlot = Slot};
	const glm::vec4 ImportedRequestedColor(0.05f, 0.31f, 0.67f, 1.0f);
	const util::UUID ImportedPreview =
		Service.BeginBaseColorPreview(ImportedDocument, std::span(&ImportedTarget, 1), ImportedRequestedColor);
	const util::UUID ImportedOperation = Service.CommitBaseColorPreview(ImportedPreview, Scheduler);
	const material::PrivateMaterialAssignmentResult ImportedResult = WaitForOperation(Service, ImportedOperation);
	util::UUID ImportedObjectID;
	{
		auto Access = ImportedDocument.GetScene().Read();
		ImportedObjectID = Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(ImportedObject)).GetPersistentID();
		const components::CObjectMeshComponent &ImportedMesh =
			Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(ImportedObject));
		Require(ImportedResult.Committed && ImportedMesh.GetMaterialOverrides().size() == 1 &&
					glm::all(glm::equal(ImportedMesh.GetMaterialOverrides().front().Material.Pin()->GetFactors().BaseColor,
										ImportedRequestedColor)),
				"private material assignment with an imported path-derived parent did not survive picker commit");
	}
	const std::filesystem::path ImportedMaterialPath =
		Content / "Generated" / "Materials" /
		(ImportedObjectID.ToString() + "_" + std::to_string(Instance.ID) + "_" + std::to_string(Slot) + ".materialinstance");
	const material::MaterialDocument ImportedPersisted = material::MaterialDocumentStore::Load(ImportedMaterialPath);
	Require(ImportedPersisted.Parent.has_value() && ImportedPersisted.Parent->ID == ImportedParentID &&
				glm::all(glm::equal(ImportedPersisted.Factors.BaseColor, ImportedRequestedColor)),
			"persisted private material lost its imported path-derived parent or selected base color");

	const util::UUID RetirementSetupOperation =
		Service.BeginBaseColorAssignment(Document, std::span(&Target, 1), RequestedColor, Scheduler);
	(void)WaitForOperation(Service, RetirementSetupOperation);
	const resource::AssetID RetirementMaterialID = PrivateMaterial.GetID();
	PrivateMaterial = {};
	Document.GetSelection().SelectOnly(ObjectID);
	Document.Execute(std::make_unique<commands::DeleteObjectsCommand>(
		Document, Document.GetSelection().GetOrdered(), [&Service, &Scheduler](std::vector<resource::AssetID> Assets)
		{ Service.QueueRetirementCandidates(std::move(Assets), Scheduler); }));
	Require(std::filesystem::is_regular_file(MaterialPath),
			"undoable object deletion retired its private material before command eviction");
	Document.Undo();
	Require(Document.GetScene().FindObject(ObjectID).IsValid() && std::filesystem::is_regular_file(MaterialPath),
			"undoing object deletion could not restore the object with its private material");
	Document.Redo();
	Document.GetHistory().Clear();
	const auto RetirementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Service.HasPendingWork() && std::chrono::steady_clock::now() < RetirementDeadline)
	{
		(void)Service.Poll(Scheduler, Registry);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const std::vector<material::PrivateMaterialAssignmentResult> RetirementResults = Service.TakeResults();
	string RetirementDiagnostic;
	for (const material::PrivateMaterialAssignmentResult &Result : RetirementResults)
		RetirementDiagnostic += " [committed=" + std::to_string(Result.Committed) + ", diagnostic=" + Result.Diagnostic + "]";
	if (const auto Snapshot = Assets.SnapshotRecord(RetirementMaterialID); Snapshot.has_value())
	{
		RetirementDiagnostic += " [record state=" + std::to_string(static_cast<uint32>(Snapshot->State)) +
								", strong=" + std::to_string(Snapshot->StrongReferences) + "]";
	}
	Require(
		!Service.HasPendingWork() && !std::filesystem::exists(MaterialPath) &&
			std::ranges::any_of(RetirementResults, [](const material::PrivateMaterialAssignmentResult &Result)
								{ return Result.Committed && Result.Diagnostic.find("Retired unused private material") != string::npos; }),
		"permanently deleted object did not retire its unreferenced private material through project trash; pending=" +
			std::to_string(Service.HasPendingWork()) + ", exists=" + std::to_string(std::filesystem::exists(MaterialPath)) +
			", results=" + RetirementDiagnostic);
}

void ValidateEditorActions()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLEditorActionValidation-" + util::UUID::GenerateRandomUUID().ToString());
	std::error_code Error;
	std::filesystem::create_directories(Root / "Content", Error);
	if (Error)
		throw std::runtime_error("Could not create editor-action validation project: " + Error.message());
	const std::filesystem::path DescriptorPath = Root / "ActionValidation.engineproject";
	{
		std::ofstream Descriptor(DescriptorPath, std::ios::binary | std::ios::trunc);
		Descriptor << "{}";
		if (!Descriptor)
			throw std::runtime_error("Could not write editor-action validation project descriptor");
	}
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code CleanupError;
			std::filesystem::remove_all(this->Root, CleanupError);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};

	EditorSession Session({.Name = "ActionValidation", .DescriptorPath = DescriptorPath});
	const pipeline::render::RenderViewID FirstView{.Value = 101};
	const pipeline::render::RenderViewID SecondView{.Value = 102};
	const pipeline::render::PickRequestID FirstViewPick =
		Session.QueueViewportPick(FirstView, 0.25f, 0.75f, viewport::SelectionOperation::Replace);
	const pipeline::render::PickRequestID SecondViewPick =
		Session.QueueViewportPick(SecondView, 0.75f, 0.25f, viewport::SelectionOperation::Replace);
	Require(FirstViewPick == SecondViewPick,
			"independent editor viewport controllers unexpectedly shared their pick-request identity sequence");
	std::vector<viewport::ViewportPickRequest> FirstViewRequests = Session.CollectViewportPickRequests(FirstView, {100, 200});
	const std::vector<viewport::ViewportPickRequest> SecondViewRequests = Session.CollectViewportPickRequests(SecondView, {200, 100});
	Require(FirstViewRequests.size() == 1 && FirstViewRequests.front().X == 25 && FirstViewRequests.front().Y == 49 &&
				SecondViewRequests.size() == 1 && SecondViewRequests.front().X == 150 && SecondViewRequests.front().Y == 74,
			"independent editor viewport pick queues did not retain their own coordinates and extents");
	viewport::EditorViewportFrame DeferredFirstView;
	DeferredFirstView.DeferredPicks.push_back(FirstViewPick);
	Session.ApplyViewportFrame(FirstView, DeferredFirstView);
	FirstViewRequests = Session.CollectViewportPickRequests(FirstView, {64, 64});
	Require(FirstViewRequests.size() == 1 && FirstViewRequests.front().Request == FirstViewPick,
			"deferring one editor viewport pick did not preserve its independent request queue");
	Session.ReleaseViewport(FirstView);
	viewport::EditorViewportFrame CompletedSecondView;
	CompletedSecondView.CompletedPicks.push_back({.Request = SecondViewPick, .Object = std::nullopt, .SourceFrame = 1});
	Session.ApplyViewportFrame(SecondView, CompletedSecondView);
	Session.ReleaseViewport(SecondView);

	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	core::diagnostics::DiagnosticSink Diagnostics(32);
	action::EditorActionContext Context{.Session = Session, .Scheduler = Scheduler, .Diagnostics = Diagnostics};
	action::EditorActionRegistry Actions;
	action::RegisterCoreEditorActions(Actions);
	Require(Actions.Snapshot().size() == 70 && Actions.Find(action::IDs::OpenScene) != nullptr &&
				Actions.Find(action::IDs::SaveScene) != nullptr && Actions.Find(action::IDs::CookProject) != nullptr &&
				Actions.Find(action::IDs::PackageProject) != nullptr && Actions.Find(action::IDs::CreateBox) != nullptr &&
				Actions.Find(action::IDs::CreatePlane) != nullptr && Actions.Find(action::IDs::UniversalTool) != nullptr &&
				Actions.Find(action::IDs::CycleTransformPivot) != nullptr && Actions.Find(action::IDs::CopyObjects) != nullptr &&
				Actions.Find(action::IDs::PasteObjects) != nullptr && Actions.Find(action::IDs::GroupObjects) != nullptr &&
				Actions.Find(action::IDs::ToggleSelectionLocked) != nullptr && Actions.Find(action::IDs::MobilityMovable) != nullptr &&
				Actions.Find(action::IDs::ViewOverdraw) != nullptr && Actions.Find(action::IDs::OverlayRenderGraph) != nullptr &&
				Actions.Find(action::IDs::Simulate) != nullptr && Actions.Find(action::IDs::Standalone) != nullptr &&
				Actions.Find(action::IDs::BuildGameModule) != nullptr && Actions.Find(action::IDs::CancelProjectBuild) != nullptr &&
				Actions.Find(action::IDs::ShowCommandReference) != nullptr && Actions.Find(action::IDs::ShowAbout) != nullptr,
			"core editor action registration is incomplete");
	Require(Actions.Invoke(action::IDs::ShowCommandReference, Context).Status == action::EditorActionStatus::Executed &&
				Session.IsCommandReferenceOpen() &&
				Actions.Invoke(action::IDs::ShowAbout, Context).Status == action::EditorActionStatus::Executed && Session.IsAboutOpen(),
			"help actions did not publish their editor-owned modal state");
	Session.SetCommandReferenceOpen(false);
	Session.SetAboutOpen(false);
	pipeline::render::ViewportSettings ActionViewportSettings;
	Context.ActiveViewportSettings = &ActionViewportSettings;
	Require(Actions.Invoke(action::IDs::ViewWireframe, Context).Status == action::EditorActionStatus::Executed &&
				ActionViewportSettings.ViewMode == pipeline::render::ViewportViewMode::Wireframe &&
				Actions.Invoke(action::IDs::OverlayGrid, Context).Status == action::EditorActionStatus::Executed &&
				ActionViewportSettings.Overlays.Grid,
			"registered viewport ribbon actions did not update the active viewport settings");

	core::input::InputSystem Input;
	Actions.InstallInput(Input);
	Require(Actions.Invoke(action::IDs::SelectTool, Context).Status == action::EditorActionStatus::Executed &&
				Actions.IsChecked(action::IDs::SelectTool, Context),
			"select toolbar action did not activate and publish its checked state");
	Require(Actions.Invoke(action::IDs::UniversalTool, Context).Status == action::EditorActionStatus::Executed &&
				Actions.IsChecked(action::IDs::UniversalTool, Context),
			"universal transform action did not activate and publish its checked state");
	Require(Actions.Invoke(action::IDs::PivotBoundingBoxCenter, Context).Status == action::EditorActionStatus::Executed &&
				Actions.IsChecked(action::IDs::PivotBoundingBoxCenter, Context),
			"bounding-box transform pivot action did not activate and publish its checked state");
	Require(Actions.Invoke(action::IDs::CycleTransformPivot, Context).Status == action::EditorActionStatus::Executed &&
				Actions.IsChecked(action::IDs::PivotWorldOrigin, Context),
			"transform pivot cycle action did not advance to the next complete pivot mode");
	const action::EditorActionResult DisabledUndo = Actions.Invoke(action::IDs::Undo, Context);
	Require(DisabledUndo.Status == action::EditorActionStatus::Disabled && !DisabledUndo.Diagnostic.empty() &&
				!Actions.GetDisabledReason(action::IDs::Undo, Context).empty(),
			"undo action executed without an undoable editor transaction");
	Require(Actions.Invoke(action::IDs::Play, Context).Status == action::EditorActionStatus::Executed &&
				Session.GetPlaySession().GetState() == play::PlaySessionState::Playing &&
				!Actions.CanExecute(action::IDs::TranslateTool, Context),
			"play action did not enter an isolated play world and disable edit-world transform actions");
	Require(Actions.Invoke(action::IDs::Pause, Context).Status == action::EditorActionStatus::Executed &&
				Session.GetPlaySession().GetState() == play::PlaySessionState::Paused && Actions.IsChecked(action::IDs::Pause, Context),
			"pause action did not toggle its play-session and checked state");
	Require(Actions.Invoke(action::IDs::Step, Context).Status == action::EditorActionStatus::Executed,
			"step action did not advance a paused play session");
	Require(Actions.Invoke(action::IDs::Play, Context).Status == action::EditorActionStatus::Executed &&
				Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped,
			"play toolbar action did not stop and discard the active play world");
	Require(Actions.Invoke(action::IDs::Simulate, Context).Status == action::EditorActionStatus::Executed &&
				Session.GetPlaySession().GetMode() == play::PlaySessionMode::Simulate && Session.GetPlaySession().HasRuntimeScene(),
			"simulate action did not enter an isolated editor-controlled world");
	Require(Actions.Invoke(action::IDs::Simulate, Context).Status == action::EditorActionStatus::Executed &&
				Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped,
			"simulate action did not stop and discard its isolated world");
	Require(Actions.Invoke(action::IDs::TranslateTool, Context).Status == action::EditorActionStatus::Executed &&
				Actions.IsChecked(action::IDs::TranslateTool, Context) && Diagnostics.Snapshot().empty(),
			"transform action did not reactivate cleanly after play stopped");
	Require(Actions.Invoke(action::IDs::CreateSphere, Context).Status == action::EditorActionStatus::Executed &&
				Session.GetDocument().GetScene().GetObjectCount() == 1,
			"primitive action did not create an authoring object");
	Require(Actions.Invoke(action::IDs::CopyObjects, Context).Status == action::EditorActionStatus::Executed &&
				Actions.CanExecute(action::IDs::PasteObjects, Context),
			"copy action did not publish a reusable complete scene-object clipboard");
	Require(Actions.Invoke(action::IDs::PasteObjects, Context).Status == action::EditorActionStatus::Executed &&
				Session.GetDocument().GetScene().GetObjectCount() == 2,
			"paste action did not execute the clipboard as an undoable scene command");
	Session.GetDocument().Undo();
	Require(Session.GetDocument().GetScene().GetObjectCount() == 1,
			"undo did not remove the scene-object subtree created by the paste action");
	const util::UUID GroupedPrimitiveID = Session.GetDocument().GetSelection().GetOrdered().front();
	Require(Actions.Invoke(action::IDs::GroupObjects, Context).Status == action::EditorActionStatus::Executed &&
				Session.GetDocument().GetScene().GetObjectCount() == 2 && Session.GetDocument().GetSelection().Size() == 1,
			"group action did not create and select one hierarchy group around the selected object");
	{
		const util::UUID GroupID = Session.GetDocument().GetSelection().GetPrimary();
		const world::ObjectHandle Group = Session.GetDocument().GetScene().FindObject(GroupID);
		const world::ObjectHandle GroupedPrimitive = Session.GetDocument().GetScene().FindObject(GroupedPrimitiveID);
		auto Access = Session.GetDocument().GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(GroupedPrimitive)).GetParent() == Group,
				"group action did not preserve the selected object beneath its new group hierarchy");
	}
	Session.GetDocument().Undo();
	Require(Session.GetDocument().GetScene().GetObjectCount() == 1 && Session.GetDocument().GetSelection().Contains(GroupedPrimitiveID),
			"group transaction undo did not atomically remove the group and restore selection");
	Require(Actions.Invoke(action::IDs::ToggleSelectionLocked, Context).Status == action::EditorActionStatus::Executed &&
				Actions.IsChecked(action::IDs::ToggleSelectionLocked, Context) &&
				Actions.Invoke(action::IDs::MobilityStatic, Context).Status == action::EditorActionStatus::Executed &&
				Actions.IsChecked(action::IDs::MobilityStatic, Context),
			"selection lock and mobility ribbon actions did not edit the selected object through registered commands");
	Require(Actions.Invoke(action::IDs::ToggleSelectionLocked, Context).Status == action::EditorActionStatus::Executed,
			"selection lock ribbon action did not unlock the selected object");
	Session.GetDocument().Undo();
	Session.GetDocument().Undo();
	Session.GetDocument().Undo();
	const util::UUID PrimitiveID = Session.GetDocument().GetSelection().GetOrdered().front();
	const world::ObjectHandle Primitive = Session.GetDocument().GetScene().FindObject(PrimitiveID);
	const world::ComponentHandle<components::CObjectMeshComponent> PrimitiveMesh =
		Session.GetDocument().GetScene().GetComponent<components::CObjectMeshComponent>(Primitive);
	Require(PrimitiveMesh.IsValid(), "primitive action did not attach a mesh component");
	resource::ModelMeshInstanceID PrimitiveMeshInstanceID = 0;
	resource::MaterialSlotID PrimitiveMaterialSlotID = 0;
	resource::AssetHandle<resource::MaterialInterfaceAsset> PrimitiveMaterial;
	{
		const auto Access = Session.GetDocument().GetScene().Read();
		const resource::AssetHandle<resource::ModelAsset> &Model = Access.Resolve(PrimitiveMesh).GetModel();
		const resource::AssetPtr<resource::ModelAsset> ModelAsset = Model.Pin();
		Require(Model && ModelAsset->GetMeshInstances().size() == 1 && ModelAsset->GetMeshInstances().front().Mesh,
				"generated primitive does not expose its deterministic model and mesh record");
		const resource::AssetPtr<resource::MeshAsset> MeshAsset = ModelAsset->GetMeshInstances().front().Mesh.Pin();
		Require(MeshAsset && MeshAsset->GetLODs().size() == 4, "generated curved primitive does not expose four deterministic LODs");
		PrimitiveMeshInstanceID = ModelAsset->GetMeshInstances().front().ID;
		PrimitiveMaterialSlotID = MeshAsset->GetMaterialSlots().front().ID;
		PrimitiveMaterial = MeshAsset->GetMaterialSlots().front().DefaultMaterial;
	}
	const std::array MaterialTargets{Primitive};
	Session.GetDocument().Execute(std::make_unique<commands::MeshMaterialOverrideCommand>(
		Session.GetDocument().GetScene(), MaterialTargets, PrimitiveMeshInstanceID, PrimitiveMaterialSlotID, PrimitiveMaterial));
	{
		auto Access = Session.GetDocument().GetScene().Read();
		Require(Access.Resolve(PrimitiveMesh).GetMaterialOverrides().size() == 1,
				"mesh material override command did not assign the selected material slot");
	}
	Session.GetDocument().Undo();
	{
		auto Access = Session.GetDocument().GetScene().Read();
		Require(Access.Resolve(PrimitiveMesh).GetMaterialOverrides().empty(),
				"mesh material override undo did not restore the inherited slot material");
	}
	Session.GetDocument().Redo();
	{
		auto Access = Session.GetDocument().GetScene().Read();
		Require(Access.Resolve(PrimitiveMesh).GetMaterialOverrides().size() == 1,
				"mesh material override redo did not restore the selected material slot");
	}
	Session.GetDocument().Undo();
	Session.CreatePrimitive(asset::PrimitiveShape::Box, PrimitiveID);
	const util::UUID ChildPrimitiveID = Session.GetDocument().GetSelection().GetPrimary();
	{
		const world::ObjectHandle Parent = Session.GetDocument().GetScene().FindObject(PrimitiveID);
		const world::ObjectHandle Child = Session.GetDocument().GetScene().FindObject(ChildPrimitiveID);
		auto Access = Session.GetDocument().GetScene().Read();
		Require(Access.Resolve(Access.GetComponent<components::CObjectHierarchyComponent>(Child)).GetParent() == Parent,
				"contextual primitive creation did not attach the new primitive beneath its requested Explorer parent");
	}
	Session.GetDocument().Undo();
	Require(Session.GetDocument().GetScene().GetObjectCount() == 1 && Session.GetDocument().GetSelection().Contains(PrimitiveID),
			"contextual primitive undo did not restore the selection that existed before creation");
	Session.GetDocument().Undo();
	Require(Session.GetDocument().GetScene().GetObjectCount() == 0, "undo did not remove the created primitive");
	Session.GetDocument().Redo();
	Require(Session.GetDocument().GetScene().GetObjectCount() == 1 && Session.GetDocument().GetScene().FindObject(PrimitiveID).IsValid(),
			"redo did not restore the primitive with its stable object identity");
	const workspace::EditorPanelState &Properties = Session.GetWorkspace().GetPanel(workspace::EditorPanelID::Properties);
	const workspace::EditorPanelState &Explorer = Session.GetWorkspace().GetPanel(workspace::EditorPanelID::Explorer);
	Require(Properties.DefaultRegion == workspace::DockRegion::Left && Explorer.DefaultRegion == workspace::DockRegion::Right &&
				Session.GetWorkspace().GetPanel(workspace::EditorPanelID::Viewport).DefaultRegion == workspace::DockRegion::Center,
			"editor workspace no longer matches the reference Properties-left, Explorer-right, viewport-center layout");
	const std::span<const workspace::EditorWorkspaceDescriptor> WorkspaceDescriptors =
		workspace::EditorWorkspace::GetWorkspaceDescriptors();
	Require(WorkspaceDescriptors.size() == static_cast<usize>(workspace::EditorWorkspaceID::Count) &&
				WorkspaceDescriptors[static_cast<usize>(workspace::EditorWorkspaceID::Model)].Name == "Model",
			"editor workspace tabs do not expose the complete typed workspace set");
	const auto FindToolbarGroup = [&Session](const string_view Name) -> const workspace::EditorToolbarGroup &
	{
		const std::span<const workspace::EditorToolbarGroup> Groups = Session.GetWorkspace().GetToolbarGroups();
		const auto Found = std::ranges::find(Groups, Name, &workspace::EditorToolbarGroup::Name);
		if (Found == Groups.end())
			throw std::runtime_error("Missing editor toolbar group '" + string(Name) + "'");
		return *Found;
	};
	Require(workspace::IsVisible(FindToolbarGroup("Transform").VisibleIn, workspace::EditorWorkspaceID::Model) &&
				workspace::IsVisible(FindToolbarGroup("Primitives").VisibleIn, workspace::EditorWorkspaceID::Model) &&
				workspace::IsVisible(FindToolbarGroup("Materials").VisibleIn, workspace::EditorWorkspaceID::Model) &&
				workspace::IsVisible(FindToolbarGroup("Import").VisibleIn, workspace::EditorWorkspaceID::Model) &&
				!workspace::IsVisible(FindToolbarGroup("Build").VisibleIn, workspace::EditorWorkspaceID::Model) &&
				!workspace::IsVisible(FindToolbarGroup("View Modes").VisibleIn, workspace::EditorWorkspaceID::Model) &&
				!workspace::IsVisible(FindToolbarGroup("Primitives").VisibleIn, workspace::EditorWorkspaceID::Home),
			"Model workspace does not select its contextual model-authoring ribbon groups");
	Require(Actions.Invoke(action::IDs::ToggleExplorer, Context).Status == action::EditorActionStatus::Executed &&
				!Session.GetWorkspace().GetPanel(workspace::EditorPanelID::Explorer).Open &&
				!Actions.IsChecked(action::IDs::ToggleExplorer, Context),
			"view action did not close the Explorer panel and update its checked state");
	Session.GetWorkspace().SetOpen(workspace::EditorPanelID::Explorer, true);
	Session.GetWorkspace().SetMinimized(workspace::EditorPanelID::Explorer, true);
	Require(Session.GetWorkspace().GetPanel(workspace::EditorPanelID::Explorer).Minimized,
			"editor workspace did not preserve a minimized resizable panel state");
	Actions.UninstallInput();
}

void ValidateEditorSerialization()
{
	std::vector<uint8> Source(256U * 1'024U);
	for (usize Index = 0; Index < Source.size(); ++Index)
		Source[Index] = static_cast<uint8>((Index * 31U + Index / 7U) & 0xFFU);
	const std::vector<uint8> Encoded = core::io::CompressedArchive::Encode(Source);
	Require(Encoded.size() < Source.size() && core::io::CompressedArchive::Decode(Encoded) == Source,
			"Zstandard cooked archive did not round-trip deterministic binary data");
	std::vector<uint8> StreamEncoded;
	usize LargestEncodeRead = 0;
	const core::io::CompressedArchiveEncodeResult EncodeResult = core::io::CompressedArchive::EncodeStream(
		Source.size(),
		[&Source, &LargestEncodeRead](const uint64 Offset, const std::span<uint8> Destination)
		{
			LargestEncodeRead = std::max(LargestEncodeRead, Destination.size());
			Require(Offset <= Source.size() && Destination.size() <= Source.size() - static_cast<usize>(Offset),
					"streaming archive encoder requested bytes outside its source");
			std::ranges::copy_n(Source.begin() + static_cast<usize>(Offset), Destination.size(), Destination.begin());
		},
		[&StreamEncoded](const uint64 Offset, const std::span<const uint8> Chunk)
		{
			Require(Offset <= std::numeric_limits<usize>::max() && Chunk.size() <= std::numeric_limits<usize>::max() - Offset,
					"streaming archive encoder produced an unaddressable output range");
			const usize Required = static_cast<usize>(Offset) + Chunk.size();
			if (StreamEncoded.size() < Required)
				StreamEncoded.resize(Required);
			std::ranges::copy(Chunk, StreamEncoded.begin() + static_cast<usize>(Offset));
		});
	Require(EncodeResult.ArchiveBytes == StreamEncoded.size() && EncodeResult.SourceBytes == Source.size() &&
				EncodeResult.Checksum == core::io::CompressedArchive::CalculateChecksum(Source) &&
				LargestEncodeRead <= 1U * 1'024U * 1'024U && core::io::CompressedArchive::Decode(StreamEncoded) == Source,
			"streaming archive encoder did not preserve fixed memory bounds and content integrity");
	std::vector<uint8> Streamed;
	usize LargestRead = 0;
	usize LargestOutput = 0;
	const core::io::CompressedArchiveDecodeResult StreamResult = core::io::CompressedArchive::DecodeStream(
		Encoded.size(),
		[&Encoded, &LargestRead](const uint64 Offset, const std::span<uint8> Destination)
		{
			LargestRead = std::max(LargestRead, Destination.size());
			Require(Offset <= Encoded.size() && Destination.size() <= Encoded.size() - static_cast<usize>(Offset),
					"streaming archive decoder requested bytes outside its encoded source");
			std::ranges::copy_n(Encoded.begin() + static_cast<usize>(Offset), Destination.size(), Destination.begin());
		},
		[&Streamed, &LargestOutput](const std::span<const uint8> Chunk)
		{
			LargestOutput = std::max(LargestOutput, Chunk.size());
			Streamed.insert(Streamed.end(), Chunk.begin(), Chunk.end());
		});
	Require(Streamed == Source && StreamResult.DecodedBytes == Source.size() &&
				StreamResult.Checksum == core::io::CompressedArchive::CalculateChecksum(Source) && LargestRead <= 1U * 1'024U * 1'024U &&
				LargestOutput <= 1U * 1'024U * 1'024U,
			"streaming archive decoder did not preserve fixed memory bounds and content integrity");
	std::vector<uint8> Bomb = Encoded;
	for (usize ByteIndex = 0; ByteIndex < sizeof(uint64); ++ByteIndex)
		Bomb[16U + ByteIndex] = 0xFFU;
	bool BombRejected = false;
	try
	{
		(void)core::io::CompressedArchive::Decode(Bomb, Source.size());
	}
	catch (const core::io::CompressedArchiveException &)
	{
		BombRejected = true;
	}
	Require(BombRejected, "compressed archive accepted an attacker-declared decoded allocation size");
	std::vector<uint8> Corrupted = Encoded;
	Corrupted.back() ^= 0x5AU;
	bool CorruptionRejected = false;
	try
	{
		(void)core::io::CompressedArchive::Decode(Corrupted);
	}
	catch (const core::io::CompressedArchiveException &)
	{
		CorruptionRejected = true;
	}
	Require(CorruptionRejected, "cooked archive accepted corrupted compressed data");

	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLSerializationValidation-" + util::UUID::GenerateRandomUUID().ToString());
	const std::filesystem::path DescriptorPath = Root / "Validation.engineproject";
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	const project::ProjectDescriptor Expected{.FormatVersion = serialization::ProjectDescriptorSerializer::CurrentFormatVersion,
											  .ID = util::UUID::GenerateRandomUUID(),
											  .Name = "Serialization Validation",
											  .DescriptorPath = DescriptorPath,
											  .StartupScene = "Scenes/Startup.enginelevel",
											  .GameModule = "Build/ValidationGame.dll"};
	serialization::ProjectDescriptorSerializer::Save(Expected);
	const project::ProjectDescriptor Loaded = serialization::ProjectDescriptorSerializer::Load(DescriptorPath);
	Require(Loaded.FormatVersion == Expected.FormatVersion && Loaded.ID == Expected.ID && Loaded.Name == Expected.Name &&
				Loaded.StartupScene == Expected.StartupScene && Loaded.GameModule == Expected.GameModule,
			"JSON project descriptor did not preserve its versioned authoring fields");
	project::ProjectDescriptor Replacement = Loaded;
	Replacement.Name = "Serialization Replacement";
	serialization::ProjectDescriptorSerializer::Save(Replacement);
	Require(serialization::ProjectDescriptorSerializer::Load(DescriptorPath).Name == Replacement.Name,
			"atomic project descriptor replacement did not publish the updated document");

	const std::filesystem::path Content = Root / "Content";
	std::filesystem::create_directories(Content);
	resource::AssetManager Assets(Content);
	reflection::ReflectionRegistry Reflection;
	reflection::RegisterCoreComponentReflection(Reflection);
	document::SceneDocument SceneDocument("Round Trip Scene");
	const world::ObjectHandle Parent = SceneDocument.CreateObject("Parent");
	const util::UUID ParentID = GetPersistentID(SceneDocument.GetScene(), Parent);
	const world::ObjectHandle Child = SceneDocument.CreateObject("Child", Parent);
	const util::UUID ChildID = GetPersistentID(SceneDocument.GetScene(), Child);
	const auto Camera = SceneDocument.GetScene().AddComponent<components::CObjectCameraComponent>(Parent);
	const auto Light = SceneDocument.GetScene().AddComponent<components::CObjectPointLightComponent>(Child);
	const auto Behaviors = SceneDocument.GetScene().AddComponent<components::CObjectBehaviorComponent>(Child);
	const auto ParentIdentity = SceneDocument.GetScene().GetComponent<components::CObjectIdentityComponent>(Parent);
	{
		auto Access = SceneDocument.GetScene().Write();
		Access.Resolve(ParentIdentity).SetTags({"Environment", "Lighting"});
		Access.Resolve(ParentIdentity).SetMobility(components::ObjectMobility::Stationary);
		Access.Resolve(ParentIdentity).SetLocked(true);
		Access.Resolve(Camera).SetVerticalFieldOfViewDegrees(72.0f);
		Access.Resolve(Light).SetLuminousPowerLumens(8'500.0f);
		components::BehaviorInstance Behavior{.Type = 77, .TypeName = "RoundTripBehavior", .SchemaVersion = 3};
		Behavior.Enabled = false;
		Behavior.Properties.emplace("Speed", float32{4.5f});
		(void)Access.Resolve(Behaviors).AddBehavior(std::move(Behavior));
	}
	const util::UUID DocumentID = SceneDocument.GetID();
	const std::filesystem::path ScenePath = Content / "RoundTrip.enginelevel";
	serialization::SceneDocumentSerializer::Save(SceneDocument, Reflection, Assets, ScenePath);
	{
		using Json = nlohmann::json;
		std::ifstream Stream(ScenePath, std::ios::binary);
		const Json Serialized = Json::parse(Stream);
		const auto ChildNode = std::ranges::find_if(Serialized.at("Objects"), [&ChildID](const Json &Object)
													{ return Object.at("ID").get<string>() == ChildID.ToString(); });
		Require(ChildNode != Serialized.at("Objects").end() && ChildNode->at("Components")
																	   .at(string(components::CObjectHierarchyComponent::ComponentName))
																	   .at("Properties")
																	   .at("Parent")
																	   .get<string>() == ParentID.ToString(),
				"scene JSON did not serialize a reflected object reference by persistent identity");
	}
	std::unique_ptr<document::SceneDocument> LoadedScene = serialization::SceneDocumentSerializer::Load(ScenePath, Reflection, Assets);
	Require(LoadedScene->GetID() == DocumentID && !LoadedScene->IsDirty() && LoadedScene->GetScene().GetObjectCount() == 2,
			"scene JSON round trip did not preserve document identity, saved state, and object count");
	const world::ObjectHandle LoadedParent = LoadedScene->GetScene().FindObject(ParentID);
	const world::ObjectHandle LoadedChild = LoadedScene->GetScene().FindObject(ChildID);
	Require(LoadedParent.IsValid() && LoadedChild.IsValid(), "scene JSON round trip did not preserve stable object identities");
	{
		auto Access = LoadedScene->GetScene().Read();
		const auto LoadedHierarchy = Access.GetComponent<components::CObjectHierarchyComponent>(LoadedChild);
		const auto LoadedIdentity = Access.GetComponent<components::CObjectIdentityComponent>(LoadedParent);
		const auto LoadedCamera = Access.GetComponent<components::CObjectCameraComponent>(LoadedParent);
		const auto LoadedLight = Access.GetComponent<components::CObjectPointLightComponent>(LoadedChild);
		const auto LoadedBehaviors = Access.GetComponent<components::CObjectBehaviorComponent>(LoadedChild);
		Require(Access.Resolve(LoadedHierarchy).GetParent() == LoadedParent &&
					std::abs(Access.Resolve(LoadedCamera).GetVerticalFieldOfViewDegrees() - 72.0f) < 1.0e-4f &&
					std::abs(Access.Resolve(LoadedLight).GetLuminousPowerLumens() - 8'500.0f) < 1.0e-4f,
				"scene JSON round trip did not restore hierarchy, camera, and light component state");
		const components::CObjectIdentityComponent &Identity = Access.Resolve(LoadedIdentity);
		Require(Identity.GetTags().size() == 2 && Identity.GetTags()[0] == "Environment" && Identity.GetTags()[1] == "Lighting" &&
					Identity.GetMobility() == components::ObjectMobility::Stationary && Identity.IsLocked(),
				"scene JSON round trip did not restore identity tags, mobility, and editor state");
		const std::vector<components::BehaviorInstance> &Instances = Access.Resolve(LoadedBehaviors).GetBehaviors();
		Require(Instances.size() == 1 && !Instances.front().Enabled && Instances.front().SchemaVersion == 3 &&
					std::get<float32>(Instances.front().Properties.at("Speed")) == 4.5f,
				"scene JSON round trip did not restore behavior identity, schema, enabled state, and properties");
	}
	{
		using Json = nlohmann::json;
		Json Future;
		{
			std::ifstream Stream(ScenePath, std::ios::binary);
			Future = Json::parse(Stream);
		}
		Future["FutureRoot"] = {{"Version", 7U}};
		Future["Objects"][0]["FutureObject"] = "retained";
		Future["Objects"][0]["Components"][string(components::CObjectIdentityComponent::ComponentName)]["Properties"]["FutureProperty"] =
			42U;
		{
			std::ofstream Stream(ScenePath, std::ios::binary | std::ios::trunc);
			Stream << Future.dump(2) << '\n';
		}
		std::unique_ptr<document::SceneDocument> ForwardCompatible =
			serialization::SceneDocumentSerializer::Load(ScenePath, Reflection, Assets);
		ForwardCompatible->SetName("Forward Compatible Scene");
		serialization::SceneDocumentSerializer::Save(*ForwardCompatible, Reflection, Assets);
		std::ifstream Stream(ScenePath, std::ios::binary);
		const Json Retained = Json::parse(Stream);
		Require(Retained.at("FutureRoot").at("Version").get<uint32>() == 7U &&
					Retained.at("Objects")[0].at("FutureObject").get<string>() == "retained" &&
					Retained.at("Objects")[0]
							.at("Components")
							.at(string(components::CObjectIdentityComponent::ComponentName))
							.at("Properties")
							.at("FutureProperty")
							.get<uint32>() == 42U,
				"scene load/save discarded unknown forward-compatible fields");

		Json Legacy = Retained;
		Legacy["FormatVersion"] = 0U;
		Legacy["EngineSchemaVersion"] = 0U;
		Legacy["Objects"][0]["Components"][string(components::CObjectIdentityComponent::ComponentName)]["SchemaVersion"] = 0U;
		{
			std::ofstream Output(ScenePath, std::ios::binary | std::ios::trunc);
			Output << Legacy.dump(2) << '\n';
		}
		uint32 RootMigrationCalls = 0;
		uint32 ComponentMigrationCalls = 0;
		serialization::SceneDocumentMigrationRegistry Migrations;
		Migrations.RegisterDocumentMigration(0U, 0U, serialization::SceneDocumentSerializer::CurrentFormatVersion,
											 serialization::SceneDocumentSerializer::CurrentEngineSchemaVersion,
											 [&RootMigrationCalls](Json &Root)
											 {
												 ++RootMigrationCalls;
												 Root["FormatVersion"] = serialization::SceneDocumentSerializer::CurrentFormatVersion;
												 Root["EngineSchemaVersion"] =
													 serialization::SceneDocumentSerializer::CurrentEngineSchemaVersion;
												 Root["MigrationData"]["LegacyRootMigrated"] = true;
											 });
		Migrations.RegisterComponentMigration(string(components::CObjectIdentityComponent::ComponentName), 0U,
											  serialization::SceneDocumentSerializer::CurrentComponentSchemaVersion,
											  [&ComponentMigrationCalls](Json &Component)
											  {
												  ++ComponentMigrationCalls;
												  Component["SchemaVersion"] =
													  serialization::SceneDocumentSerializer::CurrentComponentSchemaVersion;
											  });
		std::unique_ptr<document::SceneDocument> Migrated =
			serialization::SceneDocumentSerializer::Load(ScenePath, Reflection, Assets, 4'096U, &Migrations);
		Require(Migrated->GetID() == DocumentID && RootMigrationCalls == 1U && ComponentMigrationCalls == 1U,
				"scene migration registry did not migrate root and component schemas before construction");

		Json Cyclic = Retained;
		auto ParentNode = std::ranges::find_if(Cyclic.at("Objects"), [&ParentID](const Json &Object)
											   { return Object.at("ID").get<string>() == ParentID.ToString(); });
		auto ChildNode = std::ranges::find_if(Cyclic.at("Objects"), [&ChildID](const Json &Object)
											  { return Object.at("ID").get<string>() == ChildID.ToString(); });
		ParentNode->at("Parent") = ChildID.ToString();
		ChildNode->at("Parent") = ParentID.ToString();
		{
			std::ofstream Output(ScenePath, std::ios::binary | std::ios::trunc);
			Output << Cyclic.dump(2) << '\n';
		}
		bool CycleRejected = false;
		try
		{
			(void)serialization::SceneDocumentSerializer::Load(ScenePath, Reflection, Assets);
		}
		catch (const serialization::SceneDocumentSerializationException &)
		{
			CycleRejected = true;
		}
		Require(CycleRejected, "scene loader accepted a cyclic object hierarchy");

		Json FutureSchema = Retained;
		FutureSchema["EngineSchemaVersion"] = 2U;
		{
			std::ofstream Output(ScenePath, std::ios::binary | std::ios::trunc);
			Output << FutureSchema.dump(2) << '\n';
		}
		bool FutureSchemaRejected = false;
		try
		{
			(void)serialization::SceneDocumentSerializer::Load(ScenePath, Reflection, Assets);
		}
		catch (const serialization::SceneDocumentSerializationException &)
		{
			FutureSchemaRejected = true;
		}
		Require(FutureSchemaRejected, "scene loader accepted an unsupported future engine schema");
	}

	const std::filesystem::path TransactionModelPath = Content / "TransactionalModel.obj";
	{
		std::ofstream Output(TransactionModelPath, std::ios::binary | std::ios::trunc);
		Output << "o TransactionalTriangle\n"
				  "v 0 0 0\n"
				  "v 1 0 0\n"
				  "v 0 1 0\n"
				  "f 1 2 3\n";
		if (!Output)
			throw std::runtime_error("Could not write transactional scene asset fixture");
	}
	document::SceneDocument TransactionDocument("Transactional Asset Scene");
	const world::ObjectHandle FirstAssetObject = TransactionDocument.CreateObject("First Asset");
	const world::ObjectHandle SecondAssetObject = TransactionDocument.CreateObject("Second Asset");
	const resource::AssetHandle<resource::ModelAsset> TransactionModel =
		Assets.GetAsset<resource::ModelAsset>(TransactionModelPath.filename());
	(void)TransactionDocument.GetScene().AddComponent<components::CObjectMeshComponent>(FirstAssetObject, TransactionModel);
	(void)TransactionDocument.GetScene().AddComponent<components::CObjectMeshComponent>(SecondAssetObject, TransactionModel);
	const std::filesystem::path TransactionScenePath = Content / "TransactionalAsset.enginelevel";
	serialization::SceneDocumentSerializer::Save(TransactionDocument, Reflection, Assets, TransactionScenePath);
	{
		using Json = nlohmann::json;
		std::ifstream Input(TransactionScenePath, std::ios::binary);
		Json TransactionRoot = Json::parse(Input);
		Json &SecondModel = TransactionRoot.at("Objects")[1]
								.at("Components")
								.at(string(components::CObjectMeshComponent::ComponentName))
								.at("Properties")
								.at("Model");
		SecondModel["ID"] = resource::AssetID{};
		SecondModel["Path"] = "MissingTransactionalModel.obj";
		std::ofstream Output(TransactionScenePath, std::ios::binary | std::ios::trunc);
		Output << TransactionRoot.dump(2) << '\n';
	}
	resource::AssetManager TransactionAssets(Content);
	bool AssetResolutionFailed = false;
	try
	{
		(void)serialization::SceneDocumentSerializer::Load(TransactionScenePath, Reflection, TransactionAssets);
	}
	catch (const serialization::SceneDocumentSerializationException &)
	{
		AssetResolutionFailed = true;
	}
	Require(AssetResolutionFailed, "scene asset transaction fixture did not reject its missing second model");
	Require(TransactionAssets.SnapshotRecords().empty(),
			"failed scene asset resolution retained records imported earlier in the detached load transaction");
}

void ValidateEditorLayoutPersistence()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLEditorLayoutValidation-" + util::UUID::GenerateRandomUUID().ToString());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	const std::filesystem::path LayoutPath = Root / "Saved" / "Layouts" / "EditorLayout.json";

	workspace::EditorWorkspace Source;
	Source.SetOpen(workspace::EditorPanelID::Diagnostics, false);
	Source.SetMinimized(workspace::EditorPanelID::AssetBrowser, true);
	const std::array Viewports{
		ui::EditorViewportLayoutState{
			.View = 2, .Settings = {.ViewMode = pipeline::render::ViewportViewMode::Lit, .Overlays = {.Grid = true, .Selection = true}}},
		ui::EditorViewportLayoutState{.View = 3,
									  .Settings = {.ViewMode = pipeline::render::ViewportViewMode::Overdraw,
												   .Overlays = {.Bounds = true,
																.Skeletons = true,
																.Cameras = true,
																.Lights = true,
																.Culling = true,
																.Selection = false,
																.RenderStatistics = true,
																.RenderGraph = true}}}};
	ui::EditorLayoutStore Writer(LayoutPath);
	Writer.Capture(Source.GetPanels(), "[Docking][Data]\nDockSpace ID=0xA5A5A5A5\n", 0, 0, Viewports);
	Require(Writer.IsDirty(), "layout store did not detect changed panel or docking state");
	Writer.Flush();
	Require(!Writer.IsDirty() && std::filesystem::is_regular_file(LayoutPath),
			"layout store did not atomically publish its versioned project file");

	workspace::EditorWorkspace Restored;
	string DockingState;
	std::vector<ui::EditorViewportLayoutState> RestoredViewports;
	ui::EditorLayoutStore Reader(LayoutPath);
	Require(Reader.Load(Restored, DockingState, 0, 0, &RestoredViewports), "layout store did not load a published layout");
	Require(!Restored.GetPanel(workspace::EditorPanelID::Diagnostics).Open &&
				Restored.GetPanel(workspace::EditorPanelID::AssetBrowser).Minimized &&
				DockingState == "[Docking][Data]\nDockSpace ID=0xA5A5A5A5\n" && RestoredViewports.size() == Viewports.size() &&
				RestoredViewports[0].View == Viewports[0].View && RestoredViewports[0].Settings == Viewports[0].Settings &&
				RestoredViewports[1].View == Viewports[1].View && RestoredViewports[1].Settings == Viewports[1].Settings,
			"layout store did not restore panel visibility, docking state, viewport count, modes, and overlays");
}

void ValidateReferenceDockLayoutResizing()
{
	struct ImGuiContextScope final
	{
		ImGuiContextScope()
		{
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		}

		~ImGuiContextScope()
		{
			ImGui::DestroyContext();
		}
	};
	[[maybe_unused]] ImGuiContextScope Context;
	ImGuiIO &IO = ImGui::GetIO();
	IO.DisplaySize = ImVec2(3840.0f, 1800.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(IO.DisplaySize);
	ImGui::Begin("DockValidationHost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

	constexpr uint32 DockspaceID = 0xA5A5A5A5u;
	ui::EditorDockspace::BuildReferenceLayout(DockspaceID, 3840.0f, 1800.0f);
	ui::EditorDockspace::ResizeReferenceLayoutIfUnmodified(DockspaceID, 1920.0f, 900.0f);

	ImGuiDockNode *const Root = ImGui::DockBuilderGetNode(DockspaceID);
	const bool HasRootSplit = Root != nullptr && Root->ChildNodes[0] != nullptr && Root->ChildNodes[1] != nullptr;
	const auto SmallerChild = [](ImGuiDockNode *const Parent, const ImGuiAxis Axis)
	{
		if (Parent == nullptr || Parent->ChildNodes[0] == nullptr || Parent->ChildNodes[1] == nullptr)
			return static_cast<ImGuiDockNode *>(nullptr);
		const float32 FirstExtent = Axis == ImGuiAxis_X ? Parent->ChildNodes[0]->Size.x : Parent->ChildNodes[0]->Size.y;
		const float32 SecondExtent = Axis == ImGuiAxis_X ? Parent->ChildNodes[1]->Size.x : Parent->ChildNodes[1]->Size.y;
		return FirstExtent <= SecondExtent ? Parent->ChildNodes[0] : Parent->ChildNodes[1];
	};
	const auto LargerChild = [](ImGuiDockNode *const Parent, const ImGuiAxis Axis)
	{
		if (Parent == nullptr || Parent->ChildNodes[0] == nullptr || Parent->ChildNodes[1] == nullptr)
			return static_cast<ImGuiDockNode *>(nullptr);
		const float32 FirstExtent = Axis == ImGuiAxis_X ? Parent->ChildNodes[0]->Size.x : Parent->ChildNodes[0]->Size.y;
		const float32 SecondExtent = Axis == ImGuiAxis_X ? Parent->ChildNodes[1]->Size.x : Parent->ChildNodes[1]->Size.y;
		return FirstExtent > SecondExtent ? Parent->ChildNodes[0] : Parent->ChildNodes[1];
	};
	ImGuiDockNode *const Left = SmallerChild(Root, ImGuiAxis_X);
	ImGuiDockNode *const RightSplit = LargerChild(Root, ImGuiAxis_X);
	ImGuiDockNode *const Right = SmallerChild(RightSplit, ImGuiAxis_X);
	ImGuiDockNode *const CenterSplit = LargerChild(RightSplit, ImGuiAxis_X);
	ImGuiDockNode *const Bottom = SmallerChild(CenterSplit, ImGuiAxis_Y);
	const bool HasReferenceLeaves = Left != nullptr && Right != nullptr && CenterSplit != nullptr && Bottom != nullptr;
	const float32 RootWidth = Root == nullptr ? 0.0f : Root->Size.x;
	const float32 LeftRatio = HasReferenceLeaves && RootWidth > 0.0f ? Left->Size.x / RootWidth : 0.0f;
	const float32 RightRatio = HasReferenceLeaves && RootWidth > 0.0f ? Right->Size.x / RootWidth : 0.0f;
	const float32 BottomRatio = HasReferenceLeaves && CenterSplit->Size.y > 0.0f ? Bottom->Size.y / CenterSplit->Size.y : 0.0f;
	ImGui::End();
	ImGui::EndFrame();

	Require(HasRootSplit && HasReferenceLeaves, "reference dock layout did not retain its complete split topology after resizing");
	Require(std::abs(Root->Size.x - 1920.0f) <= 1.0f && std::abs(Root->Size.y - 900.0f) <= 1.0f,
			"reference dock layout did not adopt the resized host extent");
	Require(std::abs(LeftRatio - 0.22f) <= 0.01f && std::abs(RightRatio - 0.22f) <= 0.01f,
			"reference dock layout did not preserve equal 22-percent side panels");
	Require(std::abs(BottomRatio - 0.28f) <= 0.01f, "reference dock layout did not preserve the 28-percent bottom panel");
}

void ValidateColorEditPopupIdentity()
{
	struct ImGuiContextScope final
	{
		ImGuiContextScope()
		{
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
		}

		~ImGuiContextScope()
		{
			ImGui::DestroyContext();
		}
	};
	[[maybe_unused]] ImGuiContextScope Context;
	ImGuiIO &IO = ImGui::GetIO();
	IO.DisplaySize = ImVec2(1280.0f, 720.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	ImGui::NewFrame();
	ImGui::Begin("ColorEditPopupIdentityValidation", nullptr, ImGuiWindowFlags_NoSavedSettings);

	static constexpr std::array Labels{"Base Color", "Emissive", "##Value"};
	for (usize Index = 0; Index < Labels.size(); ++Index)
	{
		ImGui::PushID(static_cast<int32>(Index));
		ImGui::PushID(Labels[Index]);
		ImGui::OpenPopup("picker");
		ImGui::PopID();
		glm::vec4 Color(0.25f, 0.5f, 0.75f, 1.0f);
		(void)ImGui::ColorEdit4(Labels[Index], &Color.x, ImGuiColorEditFlags_Float);
		const bool PopupVisibleOutsideColorEditScope = ImGui::IsPopupOpen("picker");
		ImGui::PushID(Labels[Index]);
		const bool PopupVisibleInsideColorEditScope = ImGui::IsPopupOpen("picker");
		ImGui::PopID();
		Require(!PopupVisibleOutsideColorEditScope && PopupVisibleInsideColorEditScope,
				"color-edit picker popup identity did not remain nested under the ColorEdit label scope");
		ImGui::ClosePopupToLevel(0, true);
		ImGui::PopID();
	}

	ImGui::End();
	ImGui::EndFrame();
}

void ValidateContentBrowserPersistence()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLContentBrowserValidation-" + util::UUID::GenerateRandomUUID().ToString());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	const std::filesystem::path StatePath = Root / "Saved" / "Layouts" / "ContentBrowser.json";
	const std::vector<resource::AssetID> FavoriteAssetIDs{util::UUID::GenerateRandomUUID().ToString(),
														  util::UUID::GenerateRandomUUID().ToString()};

	const preferences::EditorContentBrowserStore Store(StatePath);
	Store.Save(FavoriteAssetIDs);
	const std::unordered_set<resource::AssetID> Restored = Store.Load();
	Require(std::filesystem::is_regular_file(StatePath) && Restored.size() == FavoriteAssetIDs.size() &&
				Restored.contains(FavoriteAssetIDs[0]) && Restored.contains(FavoriteAssetIDs[1]),
			"Content Browser state did not preserve stable asset-ID favorites");

	bool DuplicateRejected = false;
	try
	{
		const std::array DuplicateIDs{FavoriteAssetIDs[0], FavoriteAssetIDs[0]};
		Store.Save(DuplicateIDs);
	}
	catch (const std::invalid_argument &)
	{
		DuplicateRejected = true;
	}
	Require(DuplicateRejected, "Content Browser state accepted duplicate favorite asset IDs");
}

void ValidateAssetThumbnails()
{
	resource::AssetManager Assets;
	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 64});
	asset::AssetThumbnailService Thumbnails(Assets);
	const resource::AssetID ID = util::UUID::GenerateRandomUUID().ToString();
	const asset::AssetThumbnailRequest FirstRequest{
		.ID = ID, .Type = resource::AssetType::ShaderSource, .SourceHash = "thumbnail-generation-one", .Width = 64, .Height = 48};
	Thumbnails.Request(FirstRequest);
	Thumbnails.Tick(Scheduler);
	Scheduler.WaitIdle();
	Thumbnails.Tick(Scheduler);
	std::shared_ptr<const asset::AssetThumbnailImage> Image = Thumbnails.Find(FirstRequest);
	Require(Image && Image->IsValid(), "asynchronous asset thumbnail generation did not publish a valid image");
	Require(Image->Width == 64 && Image->Height == 48 && Image->Diagnostic.empty(),
			"asset thumbnail generation did not preserve its requested dimensions and success diagnostic");
	Require(std::ranges::any_of(Image->Pixels, [](const uint8 Channel) { return Channel != 0; }),
			"asset thumbnail generation produced an empty image");

	Thumbnails.Request(FirstRequest);
	Require(Thumbnails.GetPendingCount() == 0, "asset thumbnail cache queued duplicate work for the same source generation");
	const asset::AssetThumbnailRequest SecondRequest{
		.ID = ID, .Type = resource::AssetType::ShaderSource, .SourceHash = "thumbnail-generation-two", .Width = 32, .Height = 32};
	Thumbnails.Request(SecondRequest);
	Thumbnails.Tick(Scheduler);
	Scheduler.WaitIdle();
	Thumbnails.Tick(Scheduler);
	Image = Thumbnails.Find(SecondRequest);
	Require(Image && Image->IsValid() && Image->Width == 32 && Image->Height == 32,
			"asset thumbnail cache did not replace a stale source generation");

	bool InvalidDimensionsRejected = false;
	try
	{
		Thumbnails.Request({.ID = util::UUID::GenerateRandomUUID().ToString(),
							.Type = resource::AssetType::Model,
							.SourceHash = "invalid-thumbnail",
							.Width = 0,
							.Height = 32});
	}
	catch (const std::invalid_argument &)
	{
		InvalidDimensionsRejected = true;
	}
	Require(InvalidDimensionsRejected, "asset thumbnail service accepted invalid dimensions");
}

void ValidateCookPackaging()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLCookValidation-" + util::UUID::GenerateRandomUUID().ToString());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	std::filesystem::create_directories(Root / "Content" / "Data");
	std::filesystem::create_directories(Root / "RuntimeSource");
	std::filesystem::create_directories(Root / "EngineSource" / "shader");
	const std::filesystem::path DescriptorPath = Root / "Cook.engineproject";
	const project::ProjectDescriptor Descriptor{
		.Name = "CookValidation", .DescriptorPath = DescriptorPath, .StartupScene = "Data/Startup.enginelevel"};
	serialization::ProjectDescriptorSerializer::Save(Descriptor);
	{
		std::ofstream Scene(Root / "Content" / "Data" / "Startup.enginelevel", std::ios::binary);
		Scene << "{\"FormatVersion\":1,\"ID\":\"" << util::UUID::GenerateRandomUUID().ToString()
			  << "\",\"Name\":\"CookValidation\",\"Objects\":[]}";
		std::ofstream Asset(Root / "Content" / "Data" / "Payload.bin", std::ios::binary);
		for (uint32 Index = 0; Index < 16'384; ++Index)
			Asset.put(static_cast<char>(Index % 251U));
		std::ofstream Runtime(Root / "RuntimeSource" / "Game.exe", std::ios::binary);
		Runtime << "runtime fixture";
		std::ofstream Shader(Root / "EngineSource" / "shader" / "Runtime.glsl", std::ios::binary);
		Shader << "#version 460 core\n";
	}
	project::Project Project(serialization::ProjectDescriptorSerializer::Load(DescriptorPath));
	Project.CreateMissingDirectories();
	Require(std::filesystem::is_directory(Project.GetPaths().Autosaves) && std::filesystem::is_directory(Project.GetPaths().Recovery) &&
				std::filesystem::is_directory(Project.GetPaths().Layouts) &&
				std::filesystem::is_directory(Project.GetPaths().AssetRegistry) &&
				std::filesystem::is_directory(Project.GetPaths().HotReload) &&
				std::filesystem::is_directory(Project.GetPaths().DevelopmentBuild) &&
				std::filesystem::is_directory(Project.GetPaths().ShippingBuild),
			"project creation did not establish the complete authoring, intermediate, and build directory contract");
	Require(Project.ResolveVirtualPath("/Game/Data/Startup.enginelevel") == Root / "Content" / "Data" / "Startup.enginelevel" &&
				Project.MakeVirtualPath(Root / "Content" / "Data" / "Payload.bin") == "/Game/Data/Payload.bin",
			"project virtual mounts did not round-trip canonical game content paths");
	bool MountEscapeRejected = false;
	try
	{
		(void)Project.ResolveVirtualPath("/Game/../Outside.bin");
	}
	catch (const runtime::project::VirtualFileSystemException &)
	{
		MountEscapeRejected = true;
	}
	Require(MountEscapeRejected, "project virtual mount accepted a traversal path");
	core::threading::TaskScheduler Scheduler({.WorkerCount = 4, .Capacity = 128});
	cook::CookPackageService Service;
	const std::filesystem::path Output = Root / "Build" / "Packages" / "CookValidation";
	Service.Begin(Project,
				  {.OutputDirectory = Output,
				   .RuntimeFiles = {{.Source = Root / "RuntimeSource" / "Game.exe",
									 .Destination = "CookValidation.exe",
									 .Kind = runtime::project::PackageFileKind::Executable},
									{.Source = Root / "EngineSource" / "shader" / "Runtime.glsl",
									 .Destination = "Engine/shader/Runtime.glsl",
									 .Kind = runtime::project::PackageFileKind::EngineContent}}},
				  Scheduler);
	Service.Wait();
	const std::optional<cook::CookPackageResult> CookedPackage = Service.GetResult();
	Require(Service.GetState() == cook::CookPackageState::Completed && CookedPackage.has_value() && CookedPackage->Content.size() == 2 &&
				std::filesystem::is_regular_file(Output / "PackageManifest.json") &&
				std::filesystem::is_regular_file(Output / "CookValidation.exe"),
			"parallel cook/package service did not publish content, manifest, and runtime files");
	const auto PayloadEntry =
		std::ranges::find(CookedPackage->Content, std::filesystem::path("Data/Payload.bin"), &cook::CookedContentEntry::LogicalPath);
	Require(PayloadEntry != CookedPackage->Content.end(), "package manifest omitted a deterministic content entry");
	Require(PayloadEntry->ArchiveOffset > 0 && PayloadEntry->AssetID.size() == 36 && !PayloadEntry->AssetType.empty() &&
				!PayloadEntry->SourceHash.empty(),
			"package content index omitted its chunk range or asset metadata");
	const runtime::project::ProjectPackageMount Mount = runtime::project::ProjectPackage::Mount(Output, Root / "MountedCache");
	Require(Mount.ProjectID == Descriptor.ID && Mount.ProjectName == Descriptor.Name &&
				std::filesystem::is_regular_file(Mount.ContentRoot / "Data" / "Startup.enginelevel") &&
				std::filesystem::is_regular_file(Mount.ContentRoot / "Data" / "Payload.bin") &&
				std::filesystem::is_directory(Mount.EngineContentRoot / "shader"),
			"runtime package mount did not validate and materialize the packaged project");
	const std::filesystem::path TamperedContentPackage = Root / "Build" / "Packages" / "TamperedContent";
	std::filesystem::copy(Output, TamperedContentPackage, std::filesystem::copy_options::recursive);
	{
		std::fstream Chunk(TamperedContentPackage / PayloadEntry->ArchivePath, std::ios::binary | std::ios::in | std::ios::out);
		Require(static_cast<bool>(Chunk), "could not open copied package chunk for tamper validation");
		const uint64 TamperOffset = PayloadEntry->ArchiveOffset + PayloadEntry->ArchiveBytes / 2U;
		Chunk.seekg(static_cast<std::streamoff>(TamperOffset));
		char Value = 0;
		Chunk.get(Value);
		Require(static_cast<bool>(Chunk), "could not read copied package chunk for tamper validation");
		Value ^= static_cast<char>(0x5a);
		Chunk.seekp(static_cast<std::streamoff>(TamperOffset));
		Chunk.put(Value);
		Chunk.flush();
		Require(static_cast<bool>(Chunk), "could not write copied package chunk for tamper validation");
	}
	bool TamperedContentRejected = false;
	try
	{
		(void)runtime::project::ProjectPackage::Mount(TamperedContentPackage, Root / "TamperedContentCache");
	}
	catch (const runtime::project::ProjectPackageException &)
	{
		TamperedContentRejected = true;
	}
	Require(TamperedContentRejected, "runtime package mount accepted tampered compressed content");

	const std::filesystem::path TamperedRuntimePackage = Root / "Build" / "Packages" / "TamperedRuntime";
	std::filesystem::copy(Output, TamperedRuntimePackage, std::filesystem::copy_options::recursive);
	{
		std::fstream Runtime(TamperedRuntimePackage / "CookValidation.exe", std::ios::binary | std::ios::in | std::ios::out);
		Require(static_cast<bool>(Runtime), "could not open copied runtime file for tamper validation");
		char Value = 0;
		Runtime.get(Value);
		Require(static_cast<bool>(Runtime), "could not read copied runtime file for tamper validation");
		Value ^= static_cast<char>(0x3c);
		Runtime.seekp(0);
		Runtime.put(Value);
		Runtime.flush();
		Require(static_cast<bool>(Runtime), "could not write copied runtime file for tamper validation");
	}
	bool TamperedRuntimeRejected = false;
	try
	{
		(void)runtime::project::ProjectPackage::Mount(TamperedRuntimePackage, Root / "TamperedRuntimeCache");
	}
	catch (const runtime::project::ProjectPackageException &)
	{
		TamperedRuntimeRejected = true;
	}
	Require(TamperedRuntimeRejected, "runtime package mount accepted a tampered executable payload");
	bool UnsignedPackageRejected = false;
	try
	{
		(void)runtime::project::ProjectPackage::Mount(Output, Root / "RejectedUnsignedCache",
													  runtime::project::ProjectPackageTrustPolicy{.RequireSignature = true});
	}
	catch (const runtime::project::ProjectPackageException &)
	{
		UnsignedPackageRejected = true;
	}
	Require(UnsignedPackageRejected, "runtime package trust policy accepted an unsigned package while signatures were required");

	BCRYPT_ALG_HANDLE SigningAlgorithm = nullptr;
	BCRYPT_KEY_HANDLE SigningKey = nullptr;
	struct SigningCleanup final
	{
		BCRYPT_ALG_HANDLE *Algorithm = nullptr;
		BCRYPT_KEY_HANDLE *Key = nullptr;
		~SigningCleanup()
		{
			if (this->Key != nullptr && *this->Key != nullptr)
				(void)BCryptDestroyKey(*this->Key);
			if (this->Algorithm != nullptr && *this->Algorithm != nullptr)
				(void)BCryptCloseAlgorithmProvider(*this->Algorithm, 0);
		}
	} SigningCleanupScope{.Algorithm = &SigningAlgorithm, .Key = &SigningKey};
	Require(BCryptOpenAlgorithmProvider(&SigningAlgorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0 &&
				BCryptGenerateKeyPair(SigningAlgorithm, &SigningKey, 2'048U, 0) >= 0 && BCryptFinalizeKeyPair(SigningKey, 0) >= 0,
			"could not generate the package-signing validation key");
	const auto ExportKey = [&SigningKey](const wchar_t *BlobType)
	{
		ULONG RequiredBytes = 0;
		if (BCryptExportKey(SigningKey, nullptr, BlobType, nullptr, 0, &RequiredBytes, 0) < 0 || RequiredBytes == 0)
			throw std::runtime_error("Could not size a package-signing validation key blob");
		std::vector<uint8> Blob(RequiredBytes);
		if (BCryptExportKey(SigningKey, nullptr, BlobType, Blob.data(), RequiredBytes, &RequiredBytes, 0) < 0)
			throw std::runtime_error("Could not export a package-signing validation key blob");
		Blob.resize(RequiredBytes);
		return Blob;
	};
	const std::vector<uint8> PrivateKeyBlob = ExportKey(BCRYPT_RSAPRIVATE_BLOB);
	const std::vector<uint8> PublicKeyBlob = ExportKey(BCRYPT_RSAPUBLIC_BLOB);
	const std::filesystem::path PrivateKeyPath = Root / "ValidationSigningKey.blob";
	{
		std::ofstream PrivateKey(PrivateKeyPath, std::ios::binary | std::ios::trunc);
		PrivateKey.write(reinterpret_cast<const char *>(PrivateKeyBlob.data()), static_cast<std::streamsize>(PrivateKeyBlob.size()));
		Require(static_cast<bool>(PrivateKey), "could not write the package-signing validation private key");
	}
	const std::filesystem::path SignedOutput = Root / "Build" / "Packages" / "CookValidationSigned";
	Service.Reset();
	Service.Begin(Project,
				  {.OutputDirectory = SignedOutput,
				   .RuntimeFiles = {{.Source = Root / "RuntimeSource" / "Game.exe",
									 .Destination = "CookValidation.exe",
									 .Kind = runtime::project::PackageFileKind::Executable},
									{.Source = Root / "EngineSource" / "shader" / "Runtime.glsl",
									 .Destination = "Engine/shader/Runtime.glsl",
									 .Kind = runtime::project::PackageFileKind::EngineContent}},
				   .RequireSignedPackage = true,
				   .SigningKeyID = "ValidationSigningKey",
				   .SigningKeyVersion = 2U,
				   .SigningPrivateKey = PrivateKeyPath},
				  Scheduler);
	Service.Wait();
	Require(Service.GetState() == cook::CookPackageState::Completed, "signed package cook did not complete");
	const runtime::project::ProjectPackageTrustPolicy RotatingTrust{
		.RequireSignature = true,
		.TrustedKeys = {{.ID = "ValidationSigningKey", .Version = 1U, .PublicKeyBlob = PublicKeyBlob},
						{.ID = "ValidationSigningKey", .Version = 2U, .PublicKeyBlob = PublicKeyBlob}}};
	const runtime::project::ProjectPackageMount SignedMount =
		runtime::project::ProjectPackage::Mount(SignedOutput, Root / "SignedCache", RotatingTrust);
	Require(SignedMount.ProjectID == Descriptor.ID, "trusted rotated signing key did not authenticate the signed package");
	bool RetiredKeyRejected = false;
	try
	{
		(void)runtime::project::ProjectPackage::Mount(
			SignedOutput, Root / "RetiredKeyCache",
			{.RequireSignature = true, .TrustedKeys = {{.ID = "ValidationSigningKey", .Version = 1U, .PublicKeyBlob = PublicKeyBlob}}});
	}
	catch (const runtime::project::ProjectPackageException &)
	{
		RetiredKeyRejected = true;
	}
	Require(RetiredKeyRejected, "package trust policy accepted a signature from an unavailable key version");
	const std::filesystem::path TamperedSignaturePackage = Root / "Build" / "Packages" / "TamperedSignature";
	std::filesystem::copy(SignedOutput, TamperedSignaturePackage, std::filesystem::copy_options::recursive);
	{
		std::ifstream Input(TamperedSignaturePackage / "PackageManifest.json", std::ios::binary);
		nlohmann::json Manifest = nlohmann::json::parse(Input);
		string Signature = Manifest.at("Signature").at("Value").get<string>();
		Require(!Signature.empty(), "signed package manifest contained an empty signature");
		Signature.front() = Signature.front() == '0' ? '1' : '0';
		Manifest["Signature"]["Value"] = std::move(Signature);
		std::ofstream ManifestOutput(TamperedSignaturePackage / "PackageManifest.json", std::ios::binary | std::ios::trunc);
		ManifestOutput << Manifest.dump(2) << '\n';
	}
	bool TamperedSignatureRejected = false;
	try
	{
		(void)runtime::project::ProjectPackage::Mount(TamperedSignaturePackage, Root / "TamperedSignatureCache", RotatingTrust);
	}
	catch (const runtime::project::ProjectPackageException &)
	{
		TamperedSignatureRejected = true;
	}
	Require(TamperedSignatureRejected, "package mount accepted a tampered signed manifest");
	{
		std::ifstream DecodedPayload(Mount.ContentRoot / "Data" / "Payload.bin", std::ios::binary);
		DecodedPayload.seekg(1'337);
		char Value = 0;
		DecodedPayload.get(Value);
		Require(DecodedPayload && static_cast<uint8>(Value) == static_cast<uint8>(1'337 % 251U),
				"packaged Zstandard content did not decode to its original bytes");
	}
	const cook::CookPackageResult FirstPackage = *CookedPackage;
	const std::filesystem::path DeterministicOutput = Root / "Build" / "Packages" / "CookValidationDeterministic";
	Service.Reset();
	Service.Begin(Project,
				  {.OutputDirectory = DeterministicOutput,
				   .RuntimeFiles = {{.Source = Root / "RuntimeSource" / "Game.exe",
									 .Destination = "CookValidation.exe",
									 .Kind = runtime::project::PackageFileKind::Executable},
									{.Source = Root / "EngineSource" / "shader" / "Runtime.glsl",
									 .Destination = "Engine/shader/Runtime.glsl",
									 .Kind = runtime::project::PackageFileKind::EngineContent}}},
				  Scheduler);
	Service.Wait();
	const std::optional<cook::CookPackageResult> SecondPackage = Service.GetResult();
	Require(Service.GetState() == cook::CookPackageState::Completed && SecondPackage.has_value() &&
				SecondPackage->BuildID == FirstPackage.BuildID && SecondPackage->Content.size() == FirstPackage.Content.size(),
			"identical cooks did not produce the same deterministic build identity and content index size");
	for (usize Index = 0; Index < FirstPackage.Content.size(); ++Index)
	{
		const cook::CookedContentEntry &First = FirstPackage.Content[Index];
		const cook::CookedContentEntry &Second = SecondPackage->Content[Index];
		Require(First.LogicalPath == Second.LogicalPath && First.ArchivePath == Second.ArchivePath &&
					First.ArchiveOffset == Second.ArchiveOffset && First.ArchiveBytes == Second.ArchiveBytes &&
					First.ContentChecksum == Second.ContentChecksum,
				"identical cooks produced a non-deterministic archive index");
		std::ifstream FirstChunk(Output / First.ArchivePath, std::ios::binary);
		std::ifstream SecondChunk(DeterministicOutput / Second.ArchivePath, std::ios::binary);
		Require(FirstChunk && SecondChunk &&
					std::vector<char>(std::istreambuf_iterator<char>(FirstChunk), std::istreambuf_iterator<char>()) ==
						std::vector<char>(std::istreambuf_iterator<char>(SecondChunk), std::istreambuf_iterator<char>()),
				"identical cooks produced different archive chunk bytes");
	}

	{
		std::ofstream Marker(Output / "PreviousPackage.marker", std::ios::binary);
		Marker << "must survive failed replacement";
	}
	Service.Reset();
	bool InvalidReplacementRejected = false;
	try
	{
		Service.Begin(Project,
					  {.OutputDirectory = Output,
					   .RuntimeFiles = {{.Source = Root / "RuntimeSource" / "Missing.exe",
										 .Destination = "CookValidation.exe",
										 .Kind = runtime::project::PackageFileKind::Executable},
										{.Source = Root / "EngineSource" / "shader" / "Runtime.glsl",
										 .Destination = "Engine/shader/Runtime.glsl",
										 .Kind = runtime::project::PackageFileKind::EngineContent}}},
					  Scheduler);
	}
	catch (const cook::CookPackageException &)
	{
		InvalidReplacementRejected = true;
	}
	Require(InvalidReplacementRejected && std::filesystem::is_regular_file(Output / "PreviousPackage.marker") &&
				std::filesystem::is_regular_file(Output / "PackageManifest.json"),
			"invalid replacement package was not rejected before the previous completed package could be changed");
}

void ValidateEditorRecovery()
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLEditorRecoveryValidation-" + util::UUID::GenerateRandomUUID().ToString());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	std::filesystem::create_directories(Root / "Content");
	const std::filesystem::path DescriptorPath = Root / "Recovery.engineproject";
	{
		std::ofstream Descriptor(DescriptorPath, std::ios::binary | std::ios::trunc);
		Descriptor << "{}";
	}

	EditorSession Session({.Name = "RecoveryValidation", .DescriptorPath = DescriptorPath});
	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	preferences::EditorPreferences Preferences = Session.GetPreferences();
	Preferences.AutosaveIntervalSeconds = 120;
	Preferences.CommandHistoryCapacity = 2'048;
	Preferences.CameraMoveSpeed = 16.0f;
	Preferences.TransformSnappingEnabled = true;
	Preferences.TranslationSnap = 2.5f;
	Preferences.RotationSnapDegrees = 30.0f;
	Preferences.ScaleSnap = 0.25f;
	Session.SetPreferences(Preferences);
	const preferences::EditorPreferences StoredPreferences =
		preferences::EditorPreferencesStore::Load(Session.GetProject().GetPaths().Saved / "EditorPreferences.json");
	Require(StoredPreferences.AutosaveIntervalSeconds == 120 && StoredPreferences.CameraMoveSpeed == 16.0f &&
				StoredPreferences.TranslationSnap == 2.5f && StoredPreferences.RotationSnapDegrees == 30.0f &&
				StoredPreferences.ScaleSnap == 0.25f && Session.GetTransformGizmo().GetSnapSettings().Enabled &&
				Session.GetTransformGizmo().GetSnapSettings().Translation == 2.5f &&
				Session.GetDocument().GetHistory().GetCapacity() == 2'048,
			"versioned editor preferences did not persist or apply to live editor services");
	(void)Session.GetDocument().CreateObject("RecoveredObject");
	const uint64 Revision = Session.GetDocument().GetRevision();
	Session.GetRecoveryStore().Force(Session.GetDocument(), Session.GetReflection(), Session.GetProject().GetAssetManager(), Scheduler);
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Session.PollRecovery() && std::chrono::steady_clock::now() < Deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	Require(Session.GetRecoveryStore().GetLastResult().has_value() &&
				Session.GetRecoveryStore().GetLastResult()->Candidate.Revision == Revision && Session.GetDocument().IsDirty() &&
				Session.GetDocument().GetPath().empty(),
			"background recovery publication changed the authoring document save state or lost its revision");

	const std::vector<recovery::EditorRecoveryCandidate> Candidates = Session.GetRecoveryStore().Scan();
	Require(Candidates.size() == 1 && Candidates.front().DocumentID == Session.GetDocument().GetID() &&
				std::filesystem::is_regular_file(Candidates.front().SnapshotPath),
			"recovery scan did not discover the atomically published snapshot and manifest");
	Session.RecoverDocument(Candidates.front());
	Require(Session.GetDocument().GetScene().GetObjectCount() == 1 && Session.GetDocument().IsDirty() &&
				Session.GetDocument().GetPath().empty() && Session.GetRecoveryStore().Scan().empty(),
			"recovery did not replace the authoring document as dirty state and consume its recovery files");

	(void)Session.GetDocument().CreateObject("DiscardedRecoveryObject");
	Session.GetRecoveryStore().Force(Session.GetDocument(), Session.GetReflection(), Session.GetProject().GetAssetManager(), Scheduler);
	const auto DiscardDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Session.PollRecovery() && std::chrono::steady_clock::now() < DiscardDeadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	const std::vector<recovery::EditorRecoveryCandidate> DiscardCandidates = Session.GetRecoveryStore().Scan();
	Require(DiscardCandidates.size() == 1, "second recovery snapshot was not published for discard validation");
	const std::filesystem::path ExternalVictim = Root / "must-not-be-deleted.txt";
	{
		std::ofstream Victim(ExternalVictim, std::ios::binary | std::ios::trunc);
		Victim << "recovery candidate paths are not authoritative";
	}
	recovery::EditorRecoveryCandidate UntrustedCandidate = DiscardCandidates.front();
	UntrustedCandidate.SnapshotPath = ExternalVictim;
	Session.GetRecoveryStore().Discard(UntrustedCandidate);
	Require(Session.GetRecoveryStore().Scan().empty() && !std::filesystem::exists(DiscardCandidates.front().SnapshotPath) &&
				std::filesystem::is_regular_file(ExternalVictim),
			"discarding recovery trusted a caller-controlled snapshot path or did not remove the derived recovery files");
}
} // namespace

void RunDeterministicEditorCoreChecks()
{
	const auto Run = []<typename Callable>(const string_view Name, Callable &&Check)
	{
		std::cout << "[Validation] " << Name << '\n' << std::flush;
		std::forward<Callable>(Check)();
	};
	Run("authoring contracts", &ValidateAuthoringContracts);
	Run("viewport selection", &ValidateViewportSelection);
	Run("component reflection", &ValidateComponentReflection);
	Run("transform editing", &ValidateTransformEditing);
	Run("scene hierarchy", &ValidateSceneHierarchy);
	Run("scene object commands", &ValidateSceneObjectCommands);
	Run("scene cloning", &ValidateSceneCloning);
	Run("play and simulate sessions", &ValidatePlaySession);
	Run("asset registry", &ValidateAssetRegistry);
	Run("asset import", &ValidateAssetImport);
	Run("asset content operations", &ValidateAssetContentOperations);
	Run("material assets", &ValidateMaterialAssets);
	Run("model dependency security", &ValidateModelImporterSecureDependencies);
	Run("primitive assets", &ValidatePrimitiveAssets);
	Run("private material assignments", &ValidatePrivateMaterialAssignments);
	Run("editor actions", &ValidateEditorActions);
	Run("secure project paths", &ValidateSecurePaths);
	Run("editor serialization", &ValidateEditorSerialization);
	Run("editor layout persistence", &ValidateEditorLayoutPersistence);
	Run("reference dock layout resizing", &ValidateReferenceDockLayoutResizing);
	Run("color edit popup identity", &ValidateColorEditPopupIdentity);
	Run("content browser persistence", &ValidateContentBrowserPersistence);
	Run("asset thumbnails", &ValidateAssetThumbnails);
	Run("cook packaging", &ValidateCookPackaging);
	Run("editor recovery", &ValidateEditorRecovery);
}

void RunDeterministicGameModuleChecks(const std::filesystem::path &ValidModule, const std::filesystem::path &InvalidModule)
{
	const std::filesystem::path Root =
		std::filesystem::temp_directory_path() / ("OpenGLGameModuleValidation-" + util::UUID::GenerateRandomUUID().ToString());
	const std::filesystem::path Source = Root / "ValidationGame.dll";
	const std::filesystem::path Cache = Root / "Intermediate";
	std::error_code Error;
	std::filesystem::create_directories(Cache, Error);
	if (Error)
		throw std::runtime_error("Could not create game-module validation directory: " + Error.message());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code CleanupError;
			std::filesystem::remove_all(this->Root, CleanupError);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};

	std::filesystem::copy_file(ValidModule, Source, std::filesystem::copy_options::overwrite_existing, Error);
	if (Error)
		throw std::runtime_error("Could not copy valid game-module fixture: " + Error.message());
	runtime::behavior::BehaviorRegistry Registry;
	runtime::module::GameModuleManager Manager(Registry);
	Manager.Configure(Source, Cache);
	Require(Manager.ForceReload(true), "valid game module did not load through the shadow-copy manager");
	const runtime::module::GameModule *LoadedModule = Manager.GetLoadedModule();
	Require(Manager.IsLoaded() && LoadedModule != nullptr && !LoadedModule->GetName().empty() && !LoadedModule->GetBehaviors().empty(),
			"valid game module did not publish its name and registered behavior");
	const components::BehaviorTypeID RegisteredBehavior = LoadedModule->GetBehaviors().front().Type;
	const string LoadedModuleName = LoadedModule->GetName();
	Require(Registry.Contains(RegisteredBehavior), "valid game module behavior was not published to the registry");
	const std::filesystem::path LoadedShadow = LoadedModule->GetLoadedPath();
	Require(LoadedShadow != Source && std::filesystem::is_regular_file(LoadedShadow),
			"game module executed from its build output instead of a replaceable shadow copy");
	uint32 PrepareCalls = 0;
	uint32 ApplyCalls = 0;
	uint32 RollbackCalls = 0;
	const runtime::module::GameModuleReloadHooks FailingApplication{.Prepare = [&PrepareCalls]() { ++PrepareCalls; },
																	.Apply =
																		[&ApplyCalls]()
																	{
																		++ApplyCalls;
																		throw std::runtime_error(
																			"intentional replacement application failure");
																	},
																	.Rollback = [&RollbackCalls]() { ++RollbackCalls; }};
	Require(!Manager.ForceReload(true, FailingApplication) && PrepareCalls == 1 && ApplyCalls == 1 && RollbackCalls == 1 &&
				Manager.IsLoaded() && Manager.GetLoadedModule()->GetLoadedPath() == LoadedShadow && Registry.Contains(RegisteredBehavior) &&
				!Manager.GetDiagnostic().empty(),
			"failed live GameModule application did not atomically restore the previous module and registry");

	document::SceneDocument LiveDocument("GameModuleLiveReload");
	const world::ObjectHandle LiveObject = LiveDocument.CreateObject("BehaviorHost");
	const auto LiveComponent = LiveDocument.GetScene().AddComponent<components::CObjectBehaviorComponent>(LiveObject);
	const runtime::behavior::BehaviorDescriptor LiveDescriptor = *Registry.Find(RegisteredBehavior);
	{
		auto Access = LiveDocument.GetScene().Write();
		components::BehaviorInstance Instance{.Type = LiveDescriptor.Type,
											  .TypeName = LiveDescriptor.Name,
											  .ModuleName = LiveDescriptor.ModuleName,
											  .StableTypeID = LiveDescriptor.StableTypeID,
											  .SchemaVersion = LiveDescriptor.SchemaVersion};
		for (const runtime::behavior::BehaviorPropertyDescriptor &Property : LiveDescriptor.Properties)
			Instance.Properties.emplace(Property.Name, Property.DefaultValue);
		(void)Access.Resolve(LiveComponent).AddBehavior(std::move(Instance));
	}
	resource::AssetManager Assets(Root);
	play::PlaySession LiveSession(Assets, Registry);
	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	LiveSession.Start(LiveDocument.GetScene());
	LiveSession.Tick(Scheduler, 1.0 / 30.0);
	std::vector<runtime::behavior::BehaviorStateSnapshot> StateBeforeReload;
	const runtime::module::GameModuleReloadHooks LiveReload{
		.Prepare = [&LiveSession, &StateBeforeReload]() { StateBeforeReload = LiveSession.SuspendBehaviorsForReload(); },
		.Apply = [&LiveSession, &StateBeforeReload]() { LiveSession.RestoreBehaviorsAfterReload(StateBeforeReload); },
		.Rollback = [&LiveSession, &StateBeforeReload]() { LiveSession.RestoreBehaviorsAfterReload(StateBeforeReload); }};
	Require(Manager.ForceReload(true, LiveReload), "valid live GameModule replacement did not reconstruct active behavior instances");
	const std::vector<runtime::behavior::BehaviorStateSnapshot> StateAfterReload = LiveSession.SuspendBehaviorsForReload();
	Require(StateBeforeReload.size() == 1 && StateAfterReload.size() == 1 && StateBeforeReload.front().HasRuntimeState &&
				StateAfterReload.front().HasRuntimeState && StateBeforeReload.front().Data == StateAfterReload.front().Data,
			"live GameModule replacement did not preserve serialized behavior state exactly");
	LiveSession.RestoreBehaviorsAfterReload(StateAfterReload);
	LiveSession.Stop();

	std::filesystem::copy_file(InvalidModule, Source, std::filesystem::copy_options::overwrite_existing, Error);
	if (Error)
		throw std::runtime_error("Could not copy invalid game-module fixture: " + Error.message());
	const std::filesystem::file_time_type NewWriteTime = std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2);
	std::filesystem::last_write_time(Source, NewWriteTime, Error);
	if (Error)
		throw std::runtime_error("Could not advance invalid game-module fixture timestamp: " + Error.message());
	Require(!Manager.PollReload(true), "invalid game-module rebuild unexpectedly replaced the active module");
	Require(Manager.IsLoaded() && Manager.GetLoadedModule()->GetName() == LoadedModuleName && Registry.Contains(RegisteredBehavior) &&
				!Manager.GetDiagnostic().empty(),
			"failed game-module reload did not preserve the previous loaded module, registry, and diagnostic");

	Manager.Unload(true);
	Require(!Manager.IsLoaded() && !Registry.Contains(RegisteredBehavior),
			"game-module unload did not clear callbacks before releasing the library");
}

void RunDeterministicProjectBuildChecks(const std::filesystem::path &MSBuild, const std::filesystem::path &Solution,
										const std::filesystem::path &BuiltModule, string Configuration)
{
	const std::filesystem::path Root =
		Solution.parent_path() / "Intermediate" / "Validation" / ("ProjectBuild-" + util::UUID::GenerateRandomUUID().ToString());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Error;
			std::filesystem::remove_all(this->Root, Error);
		}
	};
	[[maybe_unused]] Cleanup CleanupScope{Root};
	const std::filesystem::path PublishedModule = Root / "Build" / "GameModule" / BuiltModule.filename();
	build::ProjectBuildService Service;
	Service.Configure({.MSBuildExecutable = MSBuild,
					   .Solution = Solution,
					   .Target = "GameModule",
					   .Configuration = std::move(Configuration),
					   .Platform = "x64",
					   .BuiltModule = BuiltModule,
					   .PublishedModule = PublishedModule,
					   .PackageOutputRoot = Root / "PackageBuild",
					   .EngineContentRoot = Solution.parent_path() / "OpenGL"});
	core::threading::TaskScheduler Scheduler({.WorkerCount = 2, .Capacity = 32});
	Service.BeginGameModuleBuild(Scheduler);
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
	while (!Service.Poll() && std::chrono::steady_clock::now() < Deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const std::optional<build::ProjectBuildResult> &Result = Service.GetResult();
	Require(Result.has_value() && Result->State == build::ProjectBuildState::Succeeded &&
				std::filesystem::is_regular_file(PublishedModule) && std::filesystem::file_size(PublishedModule) != 0,
			Result.has_value() ? "asynchronous GameModule build/publish failed: " + Result->Diagnostic
							   : "asynchronous GameModule build did not complete before its deadline");
	Service.BeginProjectBuild(Scheduler);
	const auto ProjectDeadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
	while (!Service.Poll() && std::chrono::steady_clock::now() < ProjectDeadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const std::optional<build::ProjectBuildResult> &ProjectResult = Service.GetResult();
	Require(ProjectResult.has_value() && ProjectResult->State == build::ProjectBuildState::Succeeded &&
				std::filesystem::is_regular_file(ProjectResult->RuntimeDirectory / "Editor.exe") &&
				std::filesystem::is_regular_file(ProjectResult->RuntimeDirectory / "EngineTools.lib") &&
				std::filesystem::is_regular_file(ProjectResult->RuntimeDirectory / "Game.exe") &&
				std::filesystem::is_regular_file(ProjectResult->RuntimeDirectory / "Validation.exe") &&
				std::filesystem::is_regular_file(ProjectResult->RuntimeDirectory / "Engine.dll"),
			ProjectResult.has_value() ? "complete project build/validation failed: " + ProjectResult->Diagnostic
									  : "complete project build did not complete before its deadline");
}
} // namespace editor::validation
