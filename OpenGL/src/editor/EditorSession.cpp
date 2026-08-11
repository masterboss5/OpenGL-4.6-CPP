#include "EditorSession.h"

#include "src/component/object/CObjectIdentityComponent.h"
#include "src/editor/commands/CreatePrimitiveCommand.h"
#include "src/editor/commands/SceneObjectCommands.h"
#include "src/editor/commands/TransformEditCommand.h"
#include "src/editor/reflection/ComponentReflection.h"
#include "src/editor/serialization/SceneDocumentSerializer.h"
#include "src/editor/play/StandaloneGameLauncher.h"
#include "src/scene/SceneTransformSnapshot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace editor
{
EditorSession::EditorSession(project::ProjectDescriptor ProjectDescriptor, string DocumentName)
	: ProjectManagerInstance(std::move(ProjectDescriptor)), AssetRegistry(this->ProjectManagerInstance.GetProject().GetPaths().Content),
	  AssetContentService(this->ProjectManagerInstance.GetProject().GetPaths().Content,
						  this->ProjectManagerInstance.GetProject().GetPaths().Intermediate,
						  this->ProjectManagerInstance.GetProject().GetPaths().Trash),
	  AssetImportService(this->ProjectManagerInstance.GetProject().GetPaths().Content,
						 this->ProjectManagerInstance.GetProject().GetPaths().Intermediate),
	  AssetReloadService(this->ProjectManagerInstance.GetProject().GetAssetManager(),
						 this->ProjectManagerInstance.GetProject().GetPaths().Content),
	  AssetThumbnailService(this->ProjectManagerInstance.GetProject().GetAssetManager()),
	  PrimitiveMeshFactory(this->ProjectManagerInstance.GetProject().GetAssetManager()),
	  Preferences(
		  preferences::EditorPreferencesStore::Load(this->ProjectManagerInstance.GetProject().GetPaths().Saved / "EditorPreferences.json")),
	  Document(std::make_unique<document::SceneDocument>(std::move(DocumentName), world::SceneCapacitySpecification{},
														 util::UUID::GenerateRandomUUID(), this->Preferences.CommandHistoryCapacity)),
	  PrivateMaterialAssignmentService(
		  this->ProjectManagerInstance.GetProject().GetAssetManager(), this->ProjectManagerInstance.GetProject().GetPaths().Content,
		  this->ProjectManagerInstance.GetProject().GetPaths().Intermediate, this->ProjectManagerInstance.GetProject().GetPaths().Trash),
	  GameModuleManager(this->BehaviorRegistry),
	  PlaySession(this->ProjectManagerInstance.GetProject().GetAssetManager(), this->BehaviorRegistry),
	  RecoveryStore(this->ProjectManagerInstance.GetProject().GetPaths().Autosaves,
					this->ProjectManagerInstance.GetProject().GetPaths().Recovery,
					{.AutosaveInterval = std::chrono::seconds(this->Preferences.AutosaveIntervalSeconds),
					 .QuietPeriod = std::chrono::seconds(this->Preferences.AutosaveQuietPeriodSeconds)})
{
	this->ProjectManagerInstance.GetProject().ValidateLayout();
	this->ProjectManagerInstance.GetProject().CreateMissingDirectories();
	this->TransformGizmo.SetSnapSettings({.Enabled = this->Preferences.TransformSnappingEnabled,
										  .Translation = this->Preferences.TranslationSnap,
										  .RotationDegrees = this->Preferences.RotationSnapDegrees,
										  .Scale = this->Preferences.ScaleSnap});
	this->AssetRegistry.StartWatching();
	reflection::RegisterCoreComponentReflection(this->Reflection);
	if (!this->ProjectManagerInstance.GetProject().GetDescriptor().GameModule.empty())
	{
		this->GameModuleManager.Configure(this->ProjectManagerInstance.GetProject().ResolveProjectPath(
											  this->ProjectManagerInstance.GetProject().GetDescriptor().GameModule),
										  this->ProjectManagerInstance.GetProject().GetPaths().HotReload);
		(void)this->GameModuleManager.PollReload(true);
	}
}

EditorSession::~EditorSession()
{
	this->AssetRegistry.StopWatching();
	try
	{
		this->PlaySession.Stop();
	}
	catch (...)
	{
	}
	if (this->TransformGizmo.IsDragging())
		this->TransformGizmo.CancelDrag();
	if (this->Document != nullptr)
		this->Document->GetHistory().Clear();
	this->WaitForBackgroundWork();
}

void EditorSession::WaitForBackgroundWork() noexcept
{
	if (this->PendingHierarchy.valid())
		this->PendingHierarchy.wait();
	this->AssetImportService.Wait();
	this->AssetContentService.Wait();
	this->AssetReloadService.Wait();
	this->PrivateMaterialAssignmentService.Shutdown();
	this->AssetThumbnailService.Wait();
	this->AssetRegistry.WaitForRefresh();
	this->ProjectBuildService.Cancel();
	this->ProjectBuildService.Wait();
	this->CookPackageService.Wait();
	this->RecoveryStore.Wait();
}

void EditorSession::RequestHierarchyRefresh(core::threading::TaskScheduler &Scheduler)
{
	if (this->PendingHierarchy.valid() || this->Hierarchy.SceneRevision == this->Document->GetRevision())
		return;
	this->PendingHierarchy =
		hierarchy::SceneHierarchyBuilder::BuildAsync(Scheduler, this->Document->GetScene(), this->Document->GetRevision());
}

void EditorSession::RequestContentRefresh(core::threading::TaskScheduler &Scheduler)
{
	this->AssetRegistry.RequestRefresh(Scheduler);
}

bool EditorSession::PollHierarchyRefresh()
{
	if (!this->PendingHierarchy.valid() || this->PendingHierarchy.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		return false;
	}

	hierarchy::SceneHierarchySnapshot Replacement = this->PendingHierarchy.get();
	if (Replacement.SceneRevision != this->Document->GetRevision())
		return false;
	this->Hierarchy = std::move(Replacement);
	this->FilteredHierarchy = hierarchy::SceneHierarchyBuilder::Filter(this->Hierarchy, this->HierarchyFilter);
	return true;
}

void EditorSession::StartPendingChangedAssetReload(core::threading::TaskScheduler &Scheduler)
{
	if (!this->ChangedAssetReloadPending || this->AssetReloadService.IsBusy())
		return;
	this->AssetReloadService.BeginChanged(Scheduler);
	this->ChangedAssetReloadPending = false;
}

bool EditorSession::PollContentRefresh(core::threading::TaskScheduler &Scheduler)
{
	if (!this->AssetRegistry.PollRefresh())
		return false;
	this->AssetRegistry.PublishTo(this->ProjectManagerInstance.GetProject().GetAssetManager());
	this->ChangedAssetReloadPending = true;
	this->StartPendingChangedAssetReload(Scheduler);
	return true;
}

bool EditorSession::PollAssetImport(core::threading::TaskScheduler &Scheduler)
{
	return this->AssetImportService.Poll(Scheduler, this->AssetRegistry);
}

bool EditorSession::PollAssetContentOperation(core::threading::TaskScheduler &Scheduler)
{
	return this->AssetContentService.Poll(Scheduler, this->AssetRegistry);
}

bool EditorSession::PollAssetReload(core::threading::TaskScheduler &Scheduler)
{
	const bool Completed = this->AssetReloadService.Poll();
	if (Completed)
		this->AssetRegistry.RequestRefresh(Scheduler, true);
	this->StartPendingChangedAssetReload(Scheduler);
	return Completed;
}

bool EditorSession::PollPrivateMaterialAssignments(core::threading::TaskScheduler &Scheduler)
{
	return this->PrivateMaterialAssignmentService.Poll(Scheduler, this->AssetRegistry);
}

void EditorSession::TickAssetThumbnails(core::threading::TaskScheduler &Scheduler)
{
	this->AssetThumbnailService.Tick(Scheduler);
}

void EditorSession::SetHierarchyFilter(string Filter)
{
	if (this->HierarchyFilter == Filter)
		return;
	this->HierarchyFilter = std::move(Filter);
	this->FilteredHierarchy = hierarchy::SceneHierarchyBuilder::Filter(this->Hierarchy, this->HierarchyFilter);
}

pipeline::render::PickRequestID EditorSession::QueueViewportPick(const pipeline::render::RenderViewID View, const float32 NormalizedX,
																 const float32 NormalizedYFromTop,
																 const viewport::SelectionOperation Operation)
{
	return this->GetViewportController(View).QueuePick(NormalizedX, NormalizedYFromTop, Operation);
}

std::vector<viewport::ViewportPickRequest> EditorSession::CollectViewportPickRequests(const pipeline::render::RenderViewID View,
																					  const core::WindowExtent Extent)
{
	std::vector<viewport::ViewportPickRequest> Requests;
	this->CollectViewportPickRequestsInto(View, Extent, Requests);
	return Requests;
}

void EditorSession::CollectViewportPickRequestsInto(const pipeline::render::RenderViewID View, const core::WindowExtent Extent,
													std::vector<viewport::ViewportPickRequest> &Requests)
{
	this->GetViewportController(View).CollectPickRequestsInto(Extent, Requests);
}

void EditorSession::ApplyViewportFrame(const pipeline::render::RenderViewID View, const viewport::EditorViewportFrame &Frame)
{
	viewport::EditorViewportController &Controller = this->GetViewportController(View);
	if (!this->PlaySession.HasRuntimeScene())
	{
		Controller.ApplyFrame(*this->Document, Frame);
		return;
	}

	viewport::EditorViewportFrame RemappedFrame = Frame;
	const world::Scene *RuntimeScene = this->PlaySession.GetRuntimeScene();
	for (viewport::ViewportPickResult &Result : RemappedFrame.CompletedPicks)
	{
		if (!Result.Object.has_value() || Result.Object->Scene != RuntimeScene->GetID() || !RuntimeScene->Contains(*Result.Object))
			continue;
		const world::ComponentHandle<components::CObjectIdentityComponent> Identity =
			RuntimeScene->GetComponent<components::CObjectIdentityComponent>(*Result.Object);
		if (!Identity.IsValid())
		{
			Result.Object.reset();
			continue;
		}
		util::UUID PersistentID;
		{
			const auto Access = RuntimeScene->Read();
			PersistentID = Access.Resolve(Identity).GetPersistentID();
		}
		const world::ObjectHandle EditObject = this->Document->GetScene().FindObject(PersistentID);
		Result.Object = EditObject.IsValid() ? std::optional<world::ObjectHandle>(EditObject) : std::nullopt;
	}
	Controller.ApplyFrame(*this->Document, RemappedFrame);
}

void EditorSession::ReleaseViewport(const pipeline::render::RenderViewID View) noexcept
{
	const auto Entry = std::ranges::find_if(this->ViewportControllers,
											[View](const ViewportControllerEntry &Candidate) { return Candidate.View == View; });
	if (Entry == this->ViewportControllers.end())
		return;
	Entry->Controller.CancelPendingPicks();
	this->ViewportControllers.erase(Entry);
}

void EditorSession::StartPlay(core::threading::TaskScheduler &Scheduler)
{
	(void)Scheduler;
	if (this->TransformGizmo.IsDragging())
		this->TransformGizmo.CancelDrag();
	this->CancelViewportPicks();
	if (this->GameModuleManager.IsConfigured())
	{
		(void)this->GameModuleManager.PollReload(true);
		if (!this->GameModuleManager.IsLoaded())
			throw play::PlaySessionException("Configured game module is not loadable: " + this->GameModuleManager.GetDiagnostic());
	}
	this->PlaySession.Start(this->Document->GetScene(), play::PlaySessionMode::Play);
}

void EditorSession::StartSimulate()
{
	if (this->TransformGizmo.IsDragging())
		this->TransformGizmo.CancelDrag();
	this->CancelViewportPicks();
	this->PlaySession.Start(this->Document->GetScene(), play::PlaySessionMode::Simulate);
}

void EditorSession::TickPlay(core::threading::TaskScheduler &Scheduler, const float64 DeltaSeconds)
{
	if (this->PlaySession.GetState() == play::PlaySessionState::Playing)
		this->PlaySession.Tick(Scheduler, DeltaSeconds);
}

void EditorSession::PausePlay()
{
	this->PlaySession.Pause();
}

void EditorSession::ResumePlay()
{
	this->PlaySession.Resume();
}

void EditorSession::StepPlay(core::threading::TaskScheduler &Scheduler)
{
	this->PlaySession.Step(Scheduler);
}

void EditorSession::StopPlay()
{
	this->CancelViewportPicks();
	this->PlaySession.Stop();
}

uint32 EditorSession::LaunchStandalone()
{
	if (this->PlaySession.GetState() != play::PlaySessionState::Stopped)
		throw play::StandaloneGameLaunchException("Stop the current play or simulation session before standalone launch");
	if (this->Document->GetPath().empty())
		throw play::StandaloneGameLaunchException("Save the current scene before standalone launch");
	if (this->Document->IsDirty())
		this->SaveDocument();
	const std::filesystem::path Scene = this->ProjectManagerInstance.GetProject().MakeContentRelative(this->Document->GetPath());
	const auto Executable =
		std::ranges::find(this->RuntimePackageFiles, runtime::project::PackageFileKind::Executable, &cook::RuntimePackageFile::Kind);
	if (Executable == this->RuntimePackageFiles.end())
		throw play::StandaloneGameLaunchException("No standalone Game executable is configured");
	const project::Project &Project = this->ProjectManagerInstance.GetProject();
	const project::ProjectDescriptor &Descriptor = Project.GetDescriptor();
	if (!Descriptor.GameModule.empty())
	{
		const std::filesystem::path GameModule = Project.ResolveProjectPath(Descriptor.GameModule);
		std::error_code Error;
		if (!std::filesystem::is_regular_file(GameModule, Error) || Error)
		{
			const string Detail = Error ? ": " + Error.message() : string{};
			throw play::StandaloneGameLaunchException(
				"Configured GameModule does not exist; build the project before standalone launch ('" + GameModule.string() + "')" +
				Detail);
		}
	}
	return play::StandaloneGameLauncher::Launch(
		{.Executable = Executable->Source, .ProjectDescriptor = Descriptor.DescriptorPath, .Scene = Scene});
}

bool EditorSession::PollGameModule()
{
	const play::PlaySessionState State = this->PlaySession.GetState();
	if ((State == play::PlaySessionState::Playing || State == play::PlaySessionState::Paused) &&
		this->PlaySession.GetMode() == play::PlaySessionMode::Play)
	{
		std::vector<runtime::behavior::BehaviorStateSnapshot> CapturedState;
		const runtime::module::GameModuleReloadHooks Hooks{
			.Prepare = [this, &CapturedState]() { CapturedState = this->PlaySession.SuspendBehaviorsForReload(); },
			.Apply = [this, &CapturedState]() { this->PlaySession.RestoreBehaviorsAfterReload(CapturedState); },
			.Rollback = [this, &CapturedState]() { this->PlaySession.RestoreBehaviorsAfterReload(CapturedState); }};
		return this->GameModuleManager.PollReload(true, Hooks);
	}
	const bool RuntimeQuiescent =
		State == play::PlaySessionState::Stopped || this->PlaySession.GetMode() == play::PlaySessionMode::Simulate;
	return this->GameModuleManager.PollReload(RuntimeQuiescent);
}

void EditorSession::ConfigureGameModuleBuild(build::GameModuleBuildSpecification Specification)
{
	this->ProjectBuildService.Configure(std::move(Specification));
}

void EditorSession::BeginGameModuleBuild(core::threading::TaskScheduler &Scheduler)
{
	this->PackageAfterBuild = false;
	this->ProjectBuildService.BeginGameModuleBuild(Scheduler);
}

bool EditorSession::PollProjectBuild(core::threading::TaskScheduler &Scheduler)
{
	if (!this->ProjectBuildService.Poll())
		return false;
	if (!this->PackageAfterBuild)
		return true;
	this->PackageAfterBuild = false;
	const std::optional<build::ProjectBuildResult> &Build = this->ProjectBuildService.GetResult();
	if (!Build.has_value() || Build->State != build::ProjectBuildState::Succeeded)
		return true;
	std::vector<cook::RuntimePackageFile> PackageFiles = this->RuntimePackageFiles;
	for (cook::RuntimePackageFile &File : PackageFiles)
	{
		switch (File.Kind)
		{
		case runtime::project::PackageFileKind::Executable:
			File.Source = Build->RuntimeDirectory / "Game.exe";
			break;
		case runtime::project::PackageFileKind::GameModule:
			File.Source = Build->RuntimeDirectory / File.Source.filename();
			break;
		case runtime::project::PackageFileKind::DynamicLibrary:
			File.Source = Build->RuntimeDirectory / File.Source.filename();
			break;
		case runtime::project::PackageFileKind::EngineContent:
			break;
		default:
			throw std::logic_error("Runtime package file kind is invalid after project build");
		}
	}
	try
	{
		this->CookPackageService.Begin(this->ProjectManagerInstance.GetProject(),
									   {.OutputDirectory = this->ProjectManagerInstance.GetProject().GetPaths().DevelopmentBuild /
														   this->ProjectManagerInstance.GetProject().GetDescriptor().Name,
										.RuntimeFiles = std::move(PackageFiles),
										.CompressionLevel = this->ProjectManagerInstance.GetProject().GetDescriptor().Cook.CompressionLevel,
										.ReplaceExisting = true},
									   Scheduler);
	}
	catch (const std::exception &Exception)
	{
		this->ProjectBuildService.ReportPostBuildFailure("Project build passed, but package staging could not start: " +
														 string(Exception.what()));
	}
	catch (...)
	{
		this->ProjectBuildService.ReportPostBuildFailure("Project build passed, but package staging failed with a non-standard exception");
	}
	return true;
}

void EditorSession::ConfigureRuntimePackage(std::vector<cook::RuntimePackageFile> Files)
{
	if (this->CookPackageService.GetState() == cook::CookPackageState::Cooking ||
		this->CookPackageService.GetState() == cook::CookPackageState::Publishing)
		throw std::logic_error("Runtime package files cannot change during an active cook");
	this->RuntimePackageFiles = std::move(Files);
}

void EditorSession::BeginCook(core::threading::TaskScheduler &Scheduler)
{
	this->CookPackageService.Begin(this->ProjectManagerInstance.GetProject(),
								   {.OutputDirectory = this->ProjectManagerInstance.GetProject().GetPaths().Cook /
													   this->ProjectManagerInstance.GetProject().GetDescriptor().Name,
									.RuntimeFiles = {},
									.CompressionLevel = this->ProjectManagerInstance.GetProject().GetDescriptor().Cook.CompressionLevel,
									.ReplaceExisting = true},
								   Scheduler);
}

void EditorSession::BeginPackage(core::threading::TaskScheduler &Scheduler)
{
	if (this->RuntimePackageFiles.empty())
		throw std::logic_error("Runnable packaging requires explicitly configured runtime files");
	if (!this->ProjectBuildService.IsConfigured())
		throw std::logic_error("Runnable packaging requires a configured complete project build");
	if (this->CookPackageService.GetState() == cook::CookPackageState::Cooking ||
		this->CookPackageService.GetState() == cook::CookPackageState::Publishing)
		throw std::logic_error("Runnable packaging cannot start while another cook is active");
	if (this->Document->GetPath().empty())
		throw std::logic_error("The active scene must be saved before packaging");
	if (this->Document->IsDirty())
		this->SaveDocument();
	this->PackageAfterBuild = true;
	try
	{
		this->ProjectBuildService.BeginProjectBuild(Scheduler);
	}
	catch (...)
	{
		this->PackageAfterBuild = false;
		throw;
	}
}

bool EditorSession::PollCookPackage()
{
	return this->CookPackageService.Poll();
}

void EditorSession::TickRecovery(core::threading::TaskScheduler &Scheduler)
{
	if (this->Preferences.AutosaveEnabled && this->PlaySession.GetState() == play::PlaySessionState::Stopped)
		this->RecoveryStore.Tick(*this->Document, this->Reflection, this->ProjectManagerInstance.GetProject().GetAssetManager(), Scheduler);
}

bool EditorSession::PollRecovery()
{
	return this->RecoveryStore.Poll();
}

void EditorSession::SaveDocument(const std::filesystem::path &Path)
{
	serialization::SceneDocumentSerializer::Save(*this->Document, this->Reflection,
												 this->ProjectManagerInstance.GetProject().GetAssetManager(), Path);
	this->RecoveryStore.AcknowledgeSaved(this->Document->GetID(), this->Document->GetRevision());
}

void EditorSession::OpenDocument(const std::filesystem::path &Path)
{
	if (this->PlaySession.GetState() != play::PlaySessionState::Stopped)
		throw std::logic_error("A scene document cannot be replaced while a play session is active");
	if (this->TransformGizmo.IsDragging())
		this->TransformGizmo.CancelDrag();
	this->CancelViewportPicks();
	if (this->PendingHierarchy.valid())
	{
		this->PendingHierarchy.wait();
		(void)this->PendingHierarchy.get();
	}
	std::unique_ptr<document::SceneDocument> Replacement = serialization::SceneDocumentSerializer::Load(
		Path, this->Reflection, this->ProjectManagerInstance.GetProject().GetAssetManager(), this->Preferences.CommandHistoryCapacity);
	this->Document = std::move(Replacement);
	this->Hierarchy = {};
	this->FilteredHierarchy = {};
	this->RecoveryStore.ResetTracking();
}

void EditorSession::RecoverDocument(const recovery::EditorRecoveryCandidate &Candidate)
{
	if (this->PlaySession.GetState() != play::PlaySessionState::Stopped)
		throw std::logic_error("A recovery snapshot cannot replace the document while a play session is active");
	const std::vector<recovery::EditorRecoveryCandidate> Available = this->RecoveryStore.Scan();
	const auto Verified =
		std::ranges::find_if(Available, [&Candidate](const recovery::EditorRecoveryCandidate &Entry)
							 { return Entry.DocumentID == Candidate.DocumentID && Entry.SnapshotPath == Candidate.SnapshotPath; });
	if (Verified == Available.end())
		throw recovery::EditorRecoveryException("Recovery candidate is no longer available");
	if (this->TransformGizmo.IsDragging())
		this->TransformGizmo.CancelDrag();
	this->CancelViewportPicks();
	if (this->PendingHierarchy.valid())
	{
		this->PendingHierarchy.wait();
		(void)this->PendingHierarchy.get();
	}
	std::unique_ptr<document::SceneDocument> Replacement = serialization::SceneDocumentSerializer::Load(
		Verified->SnapshotPath, this->Reflection, this->ProjectManagerInstance.GetProject().GetAssetManager(),
		this->Preferences.CommandHistoryCapacity);
	Replacement->MarkRecovered(Verified->OriginalPath);
	this->Document = std::move(Replacement);
	this->Hierarchy = {};
	this->FilteredHierarchy = {};
	this->RecoveryStore.Discard(*Verified);
	this->RecoveryStore.ResetTracking();
}

void EditorSession::DiscardRecovery(const recovery::EditorRecoveryCandidate &Candidate)
{
	this->RecoveryStore.Discard(Candidate);
}

std::vector<recovery::EditorRecoveryCandidate> EditorSession::ScanRecovery() const
{
	return this->RecoveryStore.Scan();
}

void EditorSession::CreatePrimitive(const asset::PrimitiveShape Shape, const util::UUID Parent)
{
	if (this->PlaySession.GetState() != play::PlaySessionState::Stopped)
		throw std::logic_error("Primitives cannot be created while a play session is active");
	this->Document->Execute(
		std::make_unique<commands::CreatePrimitiveCommand>(*this->Document, Shape, this->PrimitiveMeshFactory.GetModel(Shape), Parent));
}

void EditorSession::CopySelection()
{
	const std::vector<util::UUID> &Selection = this->Document->GetSelection().GetOrdered();
	if (Selection.empty())
		throw std::logic_error("Cannot copy an empty scene selection");
	this->ObjectClipboard = commands::SceneObjectSnapshot::Capture(*this->Document, Selection);
}

void EditorSession::PasteClipboard()
{
	if (!this->CanPasteClipboard())
		throw std::logic_error("Cannot paste because the scene-object clipboard is empty");
	this->Document->Execute(std::make_unique<commands::PasteObjectsCommand>(*this->Document, *this->ObjectClipboard));
}

bool EditorSession::CanPasteClipboard() const noexcept
{
	return this->ObjectClipboard.has_value() && !this->ObjectClipboard->Empty();
}

void EditorSession::GroupSelection()
{
	if (this->PlaySession.GetState() != play::PlaySessionState::Stopped)
		throw std::logic_error("Scene objects cannot be grouped while a play session is active");
	const std::vector<util::UUID> Selection = this->Document->GetSelection().GetOrdered();
	if (Selection.empty())
		throw std::logic_error("Cannot group an empty scene selection");

	const hierarchy::SceneHierarchySnapshot Snapshot =
		hierarchy::SceneHierarchyBuilder::Build(this->Document->GetScene(), this->Document->GetRevision());
	const std::unordered_set<util::UUID> Selected(Selection.begin(), Selection.end());
	std::vector<const hierarchy::SceneHierarchyRow *> Roots;
	for (const hierarchy::SceneHierarchyRow &Row : Snapshot.Rows)
	{
		if (!Selected.contains(Row.PersistentID))
			continue;
		uint32 Parent = Row.ParentRow;
		bool HasSelectedAncestor = false;
		while (Parent != hierarchy::InvalidHierarchyRow)
		{
			if (Selected.contains(Snapshot.Rows[Parent].PersistentID))
			{
				HasSelectedAncestor = true;
				break;
			}
			Parent = Snapshot.Rows[Parent].ParentRow;
		}
		if (!HasSelectedAncestor)
			Roots.push_back(&Row);
	}
	if (Roots.empty())
		throw std::logic_error("Scene selection did not resolve to any groupable hierarchy roots");

	const uint32 FirstParentRow = Roots.front()->ParentRow;
	const bool CommonParent =
		std::ranges::all_of(Roots, [FirstParentRow](const hierarchy::SceneHierarchyRow *Row) { return Row->ParentRow == FirstParentRow; });
	const util::UUID ParentID =
		CommonParent && FirstParentRow != hierarchy::InvalidHierarchyRow ? Snapshot.Rows[FirstParentRow].PersistentID : util::UUID{};
	const uint32 SiblingOrder = std::ranges::min(Roots, {}, &hierarchy::SceneHierarchyRow::SiblingOrder)->SiblingOrder;

	glm::vec3 WorldCenter(0.0f);
	glm::mat4 ParentWorld(1.0f);
	{
		auto Access = this->Document->GetScene().Read();
		const world::SceneTransformSnapshot WorldTransforms = world::SceneTransformSnapshot::Build(Access);
		for (const hierarchy::SceneHierarchyRow *Row : Roots)
			WorldCenter += WorldTransforms.GetPosition(Row->Object);
		WorldCenter /= static_cast<float32>(Roots.size());
		if (ParentID.IsValid())
			ParentWorld = WorldTransforms.GetMatrix(Snapshot.Rows[FirstParentRow].Object);
	}
	if (std::abs(glm::determinant(glm::mat3(ParentWorld))) <= 1.0e-6f)
		throw world::SceneException("Cannot group objects under a non-invertible parent transform");
	const glm::vec3 LocalCenter = glm::vec3(glm::inverse(ParentWorld) * glm::vec4(WorldCenter, 1.0f));

	commands::CommandHistory &History = this->Document->GetHistory();
	History.BeginTransaction("Group Objects");
	try
	{
		auto Create = std::make_unique<commands::CreateObjectCommand>(*this->Document, "Group", ParentID);
		const util::UUID GroupID = Create->GetPersistentID();
		History.Execute(std::move(Create));
		const world::ObjectHandle Group = this->Document->GetScene().FindObject(GroupID);
		const std::array TransformTarget{commands::TransformEditTarget{
			.Object = Group, .Before = {}, .After = {.Position = LocalCenter, .Rotation = {}, .Scale = glm::vec3(1.0f)}}};
		History.Execute(commands::TransformEditCommand::Create(this->Document->GetScene(), TransformTarget));
		History.Execute(std::make_unique<commands::ReparentObjectCommand>(*this->Document, GroupID, ParentID, SiblingOrder,
																		  commands::ReparentTransformRule::PreserveWorld));
		uint32 ChildOrder = 0;
		for (const hierarchy::SceneHierarchyRow *Row : Roots)
		{
			History.Execute(std::make_unique<commands::ReparentObjectCommand>(*this->Document, Row->PersistentID, GroupID, ChildOrder++,
																			  commands::ReparentTransformRule::PreserveWorld));
		}
		History.CommitTransaction();
		this->Document->GetSelection().SelectOnly(GroupID);
	}
	catch (...)
	{
		if (History.HasOpenTransaction())
			History.CancelTransaction();
		throw;
	}
}

void EditorSession::CloneSelectedPrivateMaterials(core::threading::TaskScheduler &Scheduler)
{
	std::vector<world::ObjectHandle> Objects;
	this->Document->GetSelection().ResolveInto(this->Document->GetScene(), Objects);
	(void)this->PrivateMaterialAssignmentService.ClonePrivateAssignments(*this->Document, Objects, Scheduler);
}

void EditorSession::DeleteSelectedObjects(core::threading::TaskScheduler &Scheduler)
{
	std::vector<util::UUID> Objects = this->Document->GetSelection().GetOrdered();
	if (Objects.empty())
		throw std::invalid_argument("Deleting scene objects requires a non-empty selection");
	material::PrivateMaterialAssignmentService *const Materials = &this->PrivateMaterialAssignmentService;
	this->Document->Execute(std::make_unique<commands::DeleteObjectsCommand>(
		*this->Document, std::move(Objects), [Materials, &Scheduler](std::vector<resource::AssetID> Assets)
		{ Materials->QueueRetirementCandidates(std::move(Assets), Scheduler); }));
}

project::Project &EditorSession::GetProject() noexcept
{
	return this->ProjectManagerInstance.GetProject();
}

asset::AssetContentService &EditorSession::GetAssetContentService() noexcept
{
	return this->AssetContentService;
}

asset::AssetImportService &EditorSession::GetAssetImportService() noexcept
{
	return this->AssetImportService;
}

asset::AssetReloadService &EditorSession::GetAssetReloadService() noexcept
{
	return this->AssetReloadService;
}

asset::AssetThumbnailService &EditorSession::GetAssetThumbnailService() noexcept
{
	return this->AssetThumbnailService;
}

asset::AssetRegistry &EditorSession::GetAssetRegistry() noexcept
{
	return this->AssetRegistry;
}

material::PrivateMaterialAssignmentService &EditorSession::GetPrivateMaterialAssignmentService() noexcept
{
	return this->PrivateMaterialAssignmentService;
}

document::SceneDocument &EditorSession::GetDocument() noexcept
{
	return *this->Document;
}

const document::SceneDocument &EditorSession::GetDocument() const noexcept
{
	return *this->Document;
}

reflection::ReflectionRegistry &EditorSession::GetReflection() noexcept
{
	return this->Reflection;
}

runtime::behavior::BehaviorRegistry &EditorSession::GetBehaviorRegistry() noexcept
{
	return this->BehaviorRegistry;
}

runtime::module::GameModuleManager &EditorSession::GetGameModuleManager() noexcept
{
	return this->GameModuleManager;
}

build::ProjectBuildService &EditorSession::GetProjectBuildService() noexcept
{
	return this->ProjectBuildService;
}

play::PlaySession &EditorSession::GetPlaySession() noexcept
{
	return this->PlaySession;
}

cook::CookPackageService &EditorSession::GetCookPackageService() noexcept
{
	return this->CookPackageService;
}

recovery::EditorRecoveryStore &EditorSession::GetRecoveryStore() noexcept
{
	return this->RecoveryStore;
}

const preferences::EditorPreferences &EditorSession::GetPreferences() const noexcept
{
	return this->Preferences;
}

void EditorSession::SetPreferences(preferences::EditorPreferences Preferences)
{
	Preferences.Validate();
	preferences::EditorPreferencesStore::Save(Preferences,
											  this->ProjectManagerInstance.GetProject().GetPaths().Saved / "EditorPreferences.json");
	this->Preferences = std::move(Preferences);
	this->Document->GetHistory().SetCapacity(this->Preferences.CommandHistoryCapacity);
	this->TransformGizmo.SetSnapSettings({.Enabled = this->Preferences.TransformSnappingEnabled,
										  .Translation = this->Preferences.TranslationSnap,
										  .RotationDegrees = this->Preferences.RotationSnapDegrees,
										  .Scale = this->Preferences.ScaleSnap});
	this->RecoveryStore.SetSpecification({.AutosaveInterval = std::chrono::seconds(this->Preferences.AutosaveIntervalSeconds),
										  .QuietPeriod = std::chrono::seconds(this->Preferences.AutosaveQuietPeriodSeconds)});
}

void EditorSession::SetPreferencesOpen(const bool Open) noexcept
{
	this->PreferencesOpen = Open;
}

bool EditorSession::IsPreferencesOpen() const noexcept
{
	return this->PreferencesOpen;
}

bool EditorSession::HasRuntimePackageFiles() const noexcept
{
	return !this->RuntimePackageFiles.empty();
}

workspace::EditorWorkspace &EditorSession::GetWorkspace() noexcept
{
	return this->Workspace;
}

viewport::TransformGizmoController &EditorSession::GetTransformGizmo() noexcept
{
	return this->TransformGizmo;
}

world::Scene &EditorSession::GetViewportScene() noexcept
{
	world::Scene *RuntimeScene = this->PlaySession.GetRuntimeScene();
	return RuntimeScene == nullptr ? this->Document->GetScene() : *RuntimeScene;
}

std::vector<world::ObjectHandle> EditorSession::ResolveViewportSelection() const
{
	std::vector<world::ObjectHandle> Result;
	this->ResolveViewportSelectionInto(Result);
	return Result;
}

void EditorSession::ResolveViewportSelectionInto(std::vector<world::ObjectHandle> &Result) const
{
	const std::vector<util::UUID> &Selection = this->Document->GetSelection().GetOrdered();
	if (!this->PlaySession.HasRuntimeScene())
	{
		this->Document->GetSelection().ResolveInto(this->Document->GetScene(), Result);
		return;
	}

	Result.clear();
	if (Result.capacity() < Selection.size())
		Result.reserve(Selection.size());
	for (const util::UUID &PersistentID : Selection)
	{
		const world::ObjectHandle RuntimeObject = this->PlaySession.FindRuntimeObject(PersistentID);
		if (RuntimeObject.IsValid())
			Result.push_back(RuntimeObject);
	}
}

const hierarchy::SceneHierarchySnapshot &EditorSession::GetHierarchy() const noexcept
{
	return this->FilteredHierarchy;
}

const string &EditorSession::GetHierarchyFilter() const noexcept
{
	return this->HierarchyFilter;
}

void EditorSession::RequestMaterialColorFocus() noexcept
{
	this->MaterialColorFocusRequested = true;
	this->Workspace.SetOpen(workspace::EditorPanelID::Properties, true);
}

bool EditorSession::ConsumeMaterialColorFocusRequest() noexcept
{
	return std::exchange(this->MaterialColorFocusRequested, false);
}

void EditorSession::SetCommandReferenceOpen(const bool Open) noexcept
{
	this->CommandReferenceOpen = Open;
}

bool EditorSession::IsCommandReferenceOpen() const noexcept
{
	return this->CommandReferenceOpen;
}

void EditorSession::SetAboutOpen(const bool Open) noexcept
{
	this->AboutOpen = Open;
}

bool EditorSession::IsAboutOpen() const noexcept
{
	return this->AboutOpen;
}

viewport::EditorViewportController &EditorSession::GetViewportController(const pipeline::render::RenderViewID View)
{
	if (!View.IsValid())
		throw std::invalid_argument("Viewport interaction requires a valid render-view identity");
	const auto Entry = std::ranges::find_if(this->ViewportControllers,
											[View](const ViewportControllerEntry &Candidate) { return Candidate.View == View; });
	if (Entry != this->ViewportControllers.end())
		return Entry->Controller;
	this->ViewportControllers.push_back({.View = View});
	return this->ViewportControllers.back().Controller;
}

void EditorSession::CancelViewportPicks() noexcept
{
	for (ViewportControllerEntry &Entry : this->ViewportControllers)
		Entry.Controller.CancelPendingPicks();
}
} // namespace editor
