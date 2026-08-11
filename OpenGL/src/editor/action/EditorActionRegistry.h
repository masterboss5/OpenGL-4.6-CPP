#pragma once

#include "src/core/diagnostics/DiagnosticSink.h"
#include "src/core/input/InputSystem.h"
#include "src/pipeline/render/ViewportOverlay.h"
#include "src/types.h"

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace core::threading
{
class TaskScheduler;
}

namespace core
{
class Window;
}

namespace editor
{
class EditorSession;
}

namespace editor::action
{
using EditorActionID = core::input::ActionID;

namespace IDs
{
inline constexpr EditorActionID Undo = 0xED170001ULL;
inline constexpr EditorActionID Redo = 0xED170002ULL;
inline constexpr EditorActionID SelectTool = 0xED170003ULL;
inline constexpr EditorActionID TranslateTool = 0xED170004ULL;
inline constexpr EditorActionID RotateTool = 0xED170005ULL;
inline constexpr EditorActionID ScaleTool = 0xED170006ULL;
inline constexpr EditorActionID ToggleTransformSpace = 0xED170007ULL;
inline constexpr EditorActionID ToggleTransformSnapping = 0xED170008ULL;
inline constexpr EditorActionID Play = 0xED170009ULL;
inline constexpr EditorActionID Pause = 0xED17000AULL;
inline constexpr EditorActionID Step = 0xED17000BULL;
inline constexpr EditorActionID ToggleProperties = 0xED17000CULL;
inline constexpr EditorActionID ToggleExplorer = 0xED17000DULL;
inline constexpr EditorActionID ToggleAssetBrowser = 0xED17000EULL;
inline constexpr EditorActionID ToggleOutput = 0xED17000FULL;
inline constexpr EditorActionID ImportAssets = 0xED170010ULL;
inline constexpr EditorActionID SaveScene = 0xED170011ULL;
inline constexpr EditorActionID OpenScene = 0xED170012ULL;
inline constexpr EditorActionID CookProject = 0xED170013ULL;
inline constexpr EditorActionID PackageProject = 0xED170014ULL;
inline constexpr EditorActionID CancelAssetImport = 0xED170015ULL;
inline constexpr EditorActionID ToggleDiagnostics = 0xED170016ULL;
inline constexpr EditorActionID ResetWorkspace = 0xED170017ULL;
inline constexpr EditorActionID CreateBox = 0xED170018ULL;
inline constexpr EditorActionID CreateSphere = 0xED170019ULL;
inline constexpr EditorActionID CreateCapsule = 0xED17001AULL;
inline constexpr EditorActionID CreateCylinder = 0xED17001BULL;
inline constexpr EditorActionID CreateCone = 0xED17001CULL;
inline constexpr EditorActionID CreatePlane = 0xED17001DULL;
inline constexpr EditorActionID OpenPreferences = 0xED17001EULL;
inline constexpr EditorActionID ToggleMaterialEditor = 0xED17001FULL;
inline constexpr EditorActionID DuplicateObjects = 0xED170020ULL;
inline constexpr EditorActionID DeleteObjects = 0xED170021ULL;
inline constexpr EditorActionID UniversalTool = 0xED170022ULL;
inline constexpr EditorActionID PivotActiveObject = 0xED170023ULL;
inline constexpr EditorActionID PivotMedianPoint = 0xED170024ULL;
inline constexpr EditorActionID PivotIndividualOrigins = 0xED170025ULL;
inline constexpr EditorActionID PivotBoundingBoxCenter = 0xED170026ULL;
inline constexpr EditorActionID PivotWorldOrigin = 0xED170027ULL;
inline constexpr EditorActionID CycleTransformPivot = 0xED170028ULL;
inline constexpr EditorActionID CopyObjects = 0xED170029ULL;
inline constexpr EditorActionID PasteObjects = 0xED17002AULL;
inline constexpr EditorActionID GroupObjects = 0xED17002BULL;
inline constexpr EditorActionID ToggleSelectionLocked = 0xED17002CULL;
inline constexpr EditorActionID MobilityStatic = 0xED17002DULL;
inline constexpr EditorActionID MobilityStationary = 0xED17002EULL;
inline constexpr EditorActionID MobilityMovable = 0xED17002FULL;
inline constexpr EditorActionID EditMaterialColor = 0xED170030ULL;
inline constexpr EditorActionID ViewLit = 0xED170031ULL;
inline constexpr EditorActionID ViewUnlit = 0xED170032ULL;
inline constexpr EditorActionID ViewWireframe = 0xED170033ULL;
inline constexpr EditorActionID ViewNormals = 0xED170034ULL;
inline constexpr EditorActionID ViewDepth = 0xED170035ULL;
inline constexpr EditorActionID ViewObjectID = 0xED170036ULL;
inline constexpr EditorActionID ViewOverdraw = 0xED170037ULL;
inline constexpr EditorActionID OverlayGrid = 0xED170038ULL;
inline constexpr EditorActionID OverlayBounds = 0xED170039ULL;
inline constexpr EditorActionID OverlaySkeletons = 0xED17003AULL;
inline constexpr EditorActionID OverlayCameras = 0xED17003BULL;
inline constexpr EditorActionID OverlayLights = 0xED17003CULL;
inline constexpr EditorActionID OverlayCulling = 0xED17003DULL;
inline constexpr EditorActionID OverlaySelection = 0xED17003EULL;
inline constexpr EditorActionID OverlayRenderStatistics = 0xED17003FULL;
inline constexpr EditorActionID OverlayRenderGraph = 0xED170040ULL;
inline constexpr EditorActionID Simulate = 0xED170041ULL;
inline constexpr EditorActionID Standalone = 0xED170042ULL;
inline constexpr EditorActionID BuildGameModule = 0xED170043ULL;
inline constexpr EditorActionID CancelProjectBuild = 0xED170044ULL;
inline constexpr EditorActionID ShowCommandReference = 0xED170045ULL;
inline constexpr EditorActionID ShowAbout = 0xED170046ULL;
} // namespace IDs

enum class EditorActionCategory : uint8
{
	File,
	Edit,
	Transform,
	Assets,
	Build,
	Test,
	View,
	Window,
	Help
};

struct EditorActionContext final
{
	EditorSession &Session;
	core::threading::TaskScheduler &Scheduler;
	core::diagnostics::DiagnosticSink &Diagnostics;
	core::Window *Window = nullptr;
	pipeline::render::ViewportSettings *ActiveViewportSettings = nullptr;
};

using EditorActionPredicate = std::function<bool(const EditorActionContext &)>;
using EditorActionDiagnostic = std::function<string(const EditorActionContext &)>;
using EditorActionCallback = std::function<void(EditorActionContext &)>;

struct EditorActionDescriptor final
{
	EditorActionID ID = 0;
	string Name;
	string DisplayName;
	string Description;
	string Icon;
	EditorActionCategory Category = EditorActionCategory::Edit;
	int32 Order = 0;
	std::optional<core::input::InputBinding> Shortcut;
	EditorActionPredicate CanExecute;
	EditorActionDiagnostic DisabledReason;
	EditorActionPredicate IsChecked;
	EditorActionCallback Execute;
};

enum class EditorActionStatus : uint8
{
	Executed,
	Disabled,
	Failed
};

struct EditorActionResult final
{
	EditorActionID ID = 0;
	EditorActionStatus Status = EditorActionStatus::Disabled;
	string Diagnostic;
};

class EditorActionRegistry final
{
  public:
	EditorActionRegistry() = default;
	~EditorActionRegistry();

	EditorActionRegistry(const EditorActionRegistry &) = delete;
	EditorActionRegistry &operator=(const EditorActionRegistry &) = delete;
	EditorActionRegistry(EditorActionRegistry &&) = delete;
	EditorActionRegistry &operator=(EditorActionRegistry &&) = delete;

	void Register(EditorActionDescriptor Descriptor);
	void InstallInput(core::input::InputSystem &Input);
	void UninstallInput() noexcept;
	[[nodiscard]] std::vector<EditorActionResult> ProcessInput(core::WindowID Window, EditorActionContext &Context, bool KeyboardCaptured);
	[[nodiscard]] EditorActionResult Invoke(EditorActionID ID, EditorActionContext &Context);
	void ProcessInputInto(core::WindowID Window, EditorActionContext &Context, bool KeyboardCaptured,
						  std::vector<EditorActionResult> &Results);

	[[nodiscard]] const EditorActionDescriptor *Find(EditorActionID ID) const noexcept;
	void SnapshotInto(std::vector<const EditorActionDescriptor *> &Result) const;
	[[nodiscard]] std::vector<const EditorActionDescriptor *> Snapshot() const;
	[[nodiscard]] bool CanExecute(EditorActionID ID, const EditorActionContext &Context) const;
	[[nodiscard]] string GetDisabledReason(EditorActionID ID, const EditorActionContext &Context) const;
	[[nodiscard]] bool IsChecked(EditorActionID ID, const EditorActionContext &Context) const;

  private:
	static constexpr core::input::InputContextID InputContext = 0xED170000ULL;

	std::unordered_map<EditorActionID, EditorActionDescriptor> Actions;
	std::unordered_map<string, EditorActionID> ActionsByName;
	core::input::InputSystem *InstalledInput = nullptr;
	std::vector<const EditorActionDescriptor *> InputDescriptorsScratch;
};

void RegisterCoreEditorActions(EditorActionRegistry &Registry);
} // namespace editor::action

namespace editor
{
// Menus, ribbon controls, shortcuts, and command search all resolve through
// the action registry; expose the plan's command-facing name without adding a
// second registry.
using EditorCommandRegistry = action::EditorActionRegistry;
} // namespace editor
