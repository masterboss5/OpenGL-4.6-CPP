#pragma once

#include "Source/core/threading/TaskScheduler.h"
#include "Source/editor/asset/AssetContentService.h"
#include "Source/editor/asset/AssetImportService.h"
#include "Source/editor/asset/AssetRegistry.h"
#include "Source/editor/asset/AssetReloadService.h"
#include "Source/editor/asset/AssetThumbnailService.h"
#include "Source/editor/asset/PrimitiveMeshFactory.h"
#include "Source/editor/build/ProjectBuildService.h"
#include "Source/editor/cook/CookPackageService.h"
#include "Source/editor/commands/SceneObjectCommands.h"
#include "Source/editor/document/SceneDocument.h"
#include "Source/editor/hierarchy/SceneHierarchy.h"
#include "Source/editor/material/PrivateMaterialAssignmentService.h"
#include "Source/editor/play/PlaySession.h"
#include "Source/editor/preferences/EditorPreferences.h"
#include "Source/editor/project/Project.h"
#include "Source/editor/recovery/EditorRecoveryStore.h"
#include "Source/editor/reflection/ReflectionRegistry.h"
#include "Source/editor/viewport/EditorViewportController.h"
#include "Source/editor/viewport/TransformGizmoController.h"
#include "Source/editor/workspace/EditorWorkspace.h"
#include "Source/runtime/module/GameModule.h"

#include <future>
#include <memory>
#include <optional>

namespace editor
{
class EditorSession final
{
  public:
	explicit EditorSession(project::ProjectDescriptor ProjectDescriptor, string DocumentName = "Untitled");
	~EditorSession();

	EditorSession(const EditorSession &) = delete;
	EditorSession &operator=(const EditorSession &) = delete;
	EditorSession(EditorSession &&) = delete;
	EditorSession &operator=(EditorSession &&) = delete;

	void RequestHierarchyRefresh(core::threading::TaskScheduler &Scheduler);
	void RequestContentRefresh(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool PollHierarchyRefresh();
	[[nodiscard]] bool PollContentRefresh(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool PollAssetImport(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool PollAssetContentOperation(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool PollAssetReload(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool PollPrivateMaterialAssignments(core::threading::TaskScheduler &Scheduler);
	void TickAssetThumbnails(core::threading::TaskScheduler &Scheduler);
	void WaitForBackgroundWork() noexcept;
	void SetHierarchyFilter(string Filter);

	[[nodiscard]] pipeline::render::PickRequestID QueueViewportPick(pipeline::render::RenderViewID View, float32 NormalizedX,
																	float32 NormalizedYFromTop, viewport::SelectionOperation Operation);
	[[nodiscard]] std::vector<viewport::ViewportPickRequest> CollectViewportPickRequests(pipeline::render::RenderViewID View,
																						 core::WindowExtent Extent);
	void CollectViewportPickRequestsInto(pipeline::render::RenderViewID View, core::WindowExtent Extent,
										 std::vector<viewport::ViewportPickRequest> &Requests);
	void ApplyViewportFrame(pipeline::render::RenderViewID View, const viewport::EditorViewportFrame &Frame);
	void ReleaseViewport(pipeline::render::RenderViewID View) noexcept;
	void StartPlay(core::threading::TaskScheduler &Scheduler);
	void StartSimulate();
	void TickPlay(core::threading::TaskScheduler &Scheduler, float64 DeltaSeconds);
	void PausePlay();
	void ResumePlay();
	void StepPlay(core::threading::TaskScheduler &Scheduler);
	void StopPlay();
	[[nodiscard]] uint32 LaunchStandalone();
	[[nodiscard]] bool PollGameModule();
	void ConfigureGameModuleBuild(build::GameModuleBuildSpecification Specification);
	void BeginGameModuleBuild(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool PollProjectBuild();
	void ConfigureRuntimePackage(std::vector<cook::RuntimePackageFile> Files);
	void BeginCook(core::threading::TaskScheduler &Scheduler);
	void BeginPackage(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool PollCookPackage();
	void TickRecovery(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool PollRecovery();
	void SaveDocument(const std::filesystem::path &Path = {});
	void OpenDocument(const std::filesystem::path &Path);
	void RecoverDocument(const recovery::EditorRecoveryCandidate &Candidate);
	void DiscardRecovery(const recovery::EditorRecoveryCandidate &Candidate);
	[[nodiscard]] std::vector<recovery::EditorRecoveryCandidate> ScanRecovery() const;
	void CreatePrimitive(asset::PrimitiveShape Shape, util::UUID Parent = {});
	void CopySelection();
	void PasteClipboard();
	void GroupSelection();
	void CloneSelectedPrivateMaterials(core::threading::TaskScheduler &Scheduler);
	void DeleteSelectedObjects(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool CanPasteClipboard() const noexcept;

	[[nodiscard]] project::Project &GetProject() noexcept;
	[[nodiscard]] asset::AssetContentService &GetAssetContentService() noexcept;
	[[nodiscard]] asset::AssetImportService &GetAssetImportService() noexcept;
	[[nodiscard]] asset::AssetReloadService &GetAssetReloadService() noexcept;
	[[nodiscard]] asset::AssetThumbnailService &GetAssetThumbnailService() noexcept;
	[[nodiscard]] asset::AssetRegistry &GetAssetRegistry() noexcept;
	[[nodiscard]] material::PrivateMaterialAssignmentService &GetPrivateMaterialAssignmentService() noexcept;
	[[nodiscard]] document::SceneDocument &GetDocument() noexcept;
	[[nodiscard]] const document::SceneDocument &GetDocument() const noexcept;
	[[nodiscard]] reflection::ReflectionRegistry &GetReflection() noexcept;
	[[nodiscard]] runtime::behavior::BehaviorRegistry &GetBehaviorRegistry() noexcept;
	[[nodiscard]] runtime::module::GameModuleManager &GetGameModuleManager() noexcept;
	[[nodiscard]] build::ProjectBuildService &GetProjectBuildService() noexcept;
	[[nodiscard]] play::PlaySession &GetPlaySession() noexcept;
	[[nodiscard]] cook::CookPackageService &GetCookPackageService() noexcept;
	[[nodiscard]] recovery::EditorRecoveryStore &GetRecoveryStore() noexcept;
	[[nodiscard]] const preferences::EditorPreferences &GetPreferences() const noexcept;
	void SetPreferences(preferences::EditorPreferences Preferences);
	void SetPreferencesOpen(bool Open) noexcept;
	[[nodiscard]] bool IsPreferencesOpen() const noexcept;
	[[nodiscard]] bool HasRuntimePackageFiles() const noexcept;
	[[nodiscard]] workspace::EditorWorkspace &GetWorkspace() noexcept;
	[[nodiscard]] viewport::TransformGizmoController &GetTransformGizmo() noexcept;
	[[nodiscard]] world::Scene &GetViewportScene() noexcept;
	[[nodiscard]] std::vector<world::ObjectHandle> ResolveViewportSelection() const;
	void ResolveViewportSelectionInto(std::vector<world::ObjectHandle> &Result) const;
	[[nodiscard]] const hierarchy::SceneHierarchySnapshot &GetHierarchy() const noexcept;
	[[nodiscard]] const string &GetHierarchyFilter() const noexcept;
	void RequestMaterialColorFocus() noexcept;
	[[nodiscard]] bool ConsumeMaterialColorFocusRequest() noexcept;
	void SetCommandReferenceOpen(bool Open) noexcept;
	[[nodiscard]] bool IsCommandReferenceOpen() const noexcept;
	void SetAboutOpen(bool Open) noexcept;
	[[nodiscard]] bool IsAboutOpen() const noexcept;

  private:
	struct ViewportControllerEntry final
	{
		pipeline::render::RenderViewID View;
		viewport::EditorViewportController Controller;
	};

	[[nodiscard]] viewport::EditorViewportController &GetViewportController(pipeline::render::RenderViewID View);
	void CancelViewportPicks() noexcept;

	project::ProjectManager ProjectManagerInstance;
	asset::AssetRegistry AssetRegistry;
	asset::AssetContentService AssetContentService;
	asset::AssetImportService AssetImportService;
	asset::AssetReloadService AssetReloadService;
	asset::AssetThumbnailService AssetThumbnailService;
	asset::PrimitiveMeshFactory PrimitiveMeshFactory;
	preferences::EditorPreferences Preferences;
	std::unique_ptr<document::SceneDocument> Document;
	material::PrivateMaterialAssignmentService PrivateMaterialAssignmentService;
	reflection::ReflectionRegistry Reflection;
	runtime::behavior::BehaviorRegistry BehaviorRegistry;
	runtime::module::GameModuleManager GameModuleManager;
	build::ProjectBuildService ProjectBuildService;
	play::PlaySession PlaySession;
	cook::CookPackageService CookPackageService;
	recovery::EditorRecoveryStore RecoveryStore;
	std::vector<cook::RuntimePackageFile> RuntimePackageFiles;
	workspace::EditorWorkspace Workspace;
	std::vector<ViewportControllerEntry> ViewportControllers;
	viewport::TransformGizmoController TransformGizmo;
	std::optional<commands::SceneObjectSnapshot> ObjectClipboard;
	hierarchy::SceneHierarchySnapshot Hierarchy;
	hierarchy::SceneHierarchySnapshot FilteredHierarchy;
	string HierarchyFilter;
	std::future<hierarchy::SceneHierarchySnapshot> PendingHierarchy;
	bool ChangedAssetReloadPending = false;
	bool PreferencesOpen = false;
	bool MaterialColorFocusRequested = false;
	bool CommandReferenceOpen = false;
	bool AboutOpen = false;
	void StartPendingChangedAssetReload(core::threading::TaskScheduler &Scheduler);
};
} // namespace editor
