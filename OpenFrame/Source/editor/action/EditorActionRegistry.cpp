#include "EditorActionRegistry.h"

#include "Source/core/threading/TaskScheduler.h"
#include "Source/core/window/Window.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectMeshComponent.h"
#include "Source/editor/EditorSession.h"
#include "Source/editor/commands/PropertyEditCommand.h"
#include "Source/editor/commands/SceneObjectCommands.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace editor::action
{
namespace
{
[[nodiscard]] core::input::InputBinding KeyBinding(const EditorActionID Action, const core::input::Key Key,
												   const core::input::Modifier Modifiers = core::input::Modifier::None)
{
	return {.Action = Action,
			.Source = core::input::BindingSource::Key,
			.Key = Key,
			.RequiredModifiers = Modifiers,
			.Devices = core::input::InputDeviceFilter::Keyboard};
}

[[nodiscard]] bool EditWorldAvailable(const EditorActionContext &Context)
{
	return Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped &&
		   !Context.Session.GetTransformGizmo().IsDragging();
}

[[nodiscard]] string EditWorldDisabledReason(const EditorActionContext &Context)
{
	if (Context.Session.GetPlaySession().GetState() != play::PlaySessionState::Stopped)
		return "Stop the active play or simulation session before editing the scene.";
	if (Context.Session.GetTransformGizmo().IsDragging())
		return "Finish the active viewport transform before running this command.";
	return "The edit world is not currently available.";
}

[[nodiscard]] string SelectionActionDisabledReason(const EditorActionContext &Context)
{
	if (!EditWorldAvailable(Context))
		return EditWorldDisabledReason(Context);
	return "Select at least one scene object before running this command.";
}

[[nodiscard]] string ActiveViewportDisabledReason(const EditorActionContext &)
{
	return "Activate an editor viewport before changing its rendering settings.";
}

[[nodiscard]] std::optional<std::filesystem::path> SelectScenePath(EditorActionContext &Context, const core::FileDialogOperation Operation)
{
	if (Context.Window == nullptr)
		throw std::logic_error("Scene file action requires an application window");
	const bool Saving = Operation == core::FileDialogOperation::SaveFile;
	const core::DialogResult<core::FileDialogSelection> Selection =
		Context.Window->ShowFileDialog({.Operation = Operation,
										.Title = Saving ? "Save Scene" : "Open Scene",
										.InitialDirectory = Context.Session.GetProject().GetPaths().Content,
										.InitialName = Saving ? Context.Session.GetDocument().GetName() + ".enginelevel" : "",
										.DefaultExtension = "enginelevel",
										.Filters = {{"Engine Scene", {"*.enginelevel"}}, {"All Files", {"*.*"}}},
										.RequireExistingPath = !Saving,
										.ConfirmOverwrite = true});
	if (!Selection.Accepted() || Selection.Value->Paths.empty())
		return std::nullopt;
	return Selection.Value->Paths.front();
}

[[nodiscard]] bool SaveScene(EditorActionContext &Context, const bool ForceSelection)
{
	std::filesystem::path Path = Context.Session.GetDocument().GetPath();
	if (ForceSelection || Path.empty())
	{
		const std::optional<std::filesystem::path> Selected = SelectScenePath(Context, core::FileDialogOperation::SaveFile);
		if (!Selected.has_value())
			return false;
		Path = *Selected;
	}
	Context.Session.SaveDocument(Path);
	return true;
}

void EditSelectedIdentityProperty(EditorActionContext &Context, const string_view PropertyName, reflection::PropertyValue Value)
{
	const std::optional<reflection::TypeDescriptor> Descriptor =
		Context.Session.GetReflection().Find("components." + string(components::CObjectIdentityComponent::ComponentName));
	if (!Descriptor.has_value())
		throw std::logic_error("Identity reflection descriptor is unavailable");
	const auto Property = std::ranges::find(Descriptor->Properties, PropertyName, &reflection::PropertyDescriptor::Name);
	if (Property == Descriptor->Properties.end() || !Property->Write)
		throw std::logic_error("Identity property is not writable: " + string(PropertyName));

	world::Scene &Scene = Context.Session.GetDocument().GetScene();
	std::vector<world::ObjectHandle> Targets;
	for (const util::UUID &ID : Context.Session.GetDocument().GetSelection().GetOrdered())
	{
		const world::ObjectHandle Object = Scene.FindObject(ID);
		if (!Object.IsValid())
			throw std::out_of_range("Selected object no longer exists");
		Targets.push_back(Object);
	}
	Context.Session.GetDocument().Execute(
		commands::PropertyEditCommand::Create(Scene, Targets, components::CObjectIdentityComponent::TypeID, *Property, std::move(Value),
											  &Context.Session.GetProject().GetAssetManager()));
}

[[nodiscard]] std::optional<bool> GetPrimaryLocked(const EditorActionContext &Context)
{
	const document::SelectionSet &Selection = Context.Session.GetDocument().GetSelection();
	if (Selection.Empty())
		return std::nullopt;
	world::Scene &Scene = Context.Session.GetDocument().GetScene();
	const world::ObjectHandle Object = Scene.FindObject(Selection.GetPrimary());
	if (!Object.IsValid())
		return std::nullopt;
	auto Access = Scene.Read();
	return Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(Object)).IsLocked();
}

[[nodiscard]] std::optional<components::ObjectMobility> GetPrimaryMobility(const EditorActionContext &Context)
{
	const document::SelectionSet &Selection = Context.Session.GetDocument().GetSelection();
	if (Selection.Empty())
		return std::nullopt;
	world::Scene &Scene = Context.Session.GetDocument().GetScene();
	const world::ObjectHandle Object = Scene.FindObject(Selection.GetPrimary());
	if (!Object.IsValid())
		return std::nullopt;
	auto Access = Scene.Read();
	return Access.Resolve(Access.GetComponent<components::CObjectIdentityComponent>(Object)).GetMobility();
}

[[nodiscard]] bool HasSelectedMesh(const EditorActionContext &Context)
{
	const document::SelectionSet &Selection = Context.Session.GetDocument().GetSelection();
	if (Selection.Empty())
		return false;
	world::Scene &Scene = Context.Session.GetDocument().GetScene();
	const world::ObjectHandle Object = Scene.FindObject(Selection.GetPrimary());
	return Object.IsValid() && Scene.GetComponent<components::CObjectMeshComponent>(Object).IsValid();
}
} // namespace

EditorActionRegistry::~EditorActionRegistry()
{
	this->UninstallInput();
}

void EditorActionRegistry::Register(EditorActionDescriptor Descriptor)
{
	if (this->InstalledInput != nullptr)
		throw std::logic_error("Editor actions cannot register after input installation");
	if (Descriptor.ID == 0 || Descriptor.Name.empty() || Descriptor.DisplayName.empty() || !Descriptor.Execute)
		throw std::invalid_argument("Editor action requires a non-zero identity, names, and execution callback");
	if (this->Actions.contains(Descriptor.ID))
		throw std::invalid_argument("Editor action identity is already registered");
	if (this->ActionsByName.contains(Descriptor.Name))
		throw std::invalid_argument("Editor action name '" + Descriptor.Name + "' is already registered");
	if (Descriptor.Shortcut.has_value())
		Descriptor.Shortcut->Action = Descriptor.ID;
	this->ActionsByName.emplace(Descriptor.Name, Descriptor.ID);
	this->Actions.emplace(Descriptor.ID, std::move(Descriptor));
}

void EditorActionRegistry::InstallInput(core::input::InputSystem &Input)
{
	if (this->InstalledInput != nullptr)
		throw std::logic_error("Editor action input is already installed");
	Input.CreateContext(EditorActionRegistry::InputContext, 10'000);
	try
	{
		for (const auto &[ID, Descriptor] : this->Actions)
		{
			Input.DefineAction({.ID = ID, .ValueType = core::input::ActionValueType::Boolean});
			if (Descriptor.Shortcut.has_value())
				Input.AddBinding(EditorActionRegistry::InputContext, *Descriptor.Shortcut);
		}
	}
	catch (...)
	{
		for (const auto &[ID, Descriptor] : this->Actions)
		{
			(void)Descriptor;
			try
			{
				Input.RemoveAction(ID);
			}
			catch (...)
			{
			}
		}
		try
		{
			Input.RemoveContext(EditorActionRegistry::InputContext);
		}
		catch (...)
		{
		}
		throw;
	}
	this->InstalledInput = &Input;
}

void EditorActionRegistry::UninstallInput() noexcept
{
	if (this->InstalledInput == nullptr)
		return;
	for (const auto &[ID, Descriptor] : this->Actions)
	{
		(void)Descriptor;
		try
		{
			this->InstalledInput->RemoveAction(ID);
		}
		catch (...)
		{
		}
	}
	try
	{
		this->InstalledInput->RemoveContext(EditorActionRegistry::InputContext);
	}
	catch (...)
	{
	}
	this->InstalledInput = nullptr;
}

std::vector<EditorActionResult> EditorActionRegistry::ProcessInput(const core::WindowID Window, EditorActionContext &Context,
																   const bool KeyboardCaptured)
{
	std::vector<EditorActionResult> Results;
	this->ProcessInputInto(Window, Context, KeyboardCaptured, Results);
	return Results;
}

void EditorActionRegistry::ProcessInputInto(const core::WindowID Window, EditorActionContext &Context, const bool KeyboardCaptured,
											std::vector<EditorActionResult> &Results)
{
	if (this->InstalledInput == nullptr)
		throw std::logic_error("Editor action input must be installed before processing");
	Results.clear();
	if (KeyboardCaptured)
		return;
	this->SnapshotInto(this->InputDescriptorsScratch);
	for (const EditorActionDescriptor *Descriptor : this->InputDescriptorsScratch)
	{
		if (!Descriptor->Shortcut.has_value() || !this->InstalledInput->GetAction(Descriptor->ID, Window).Triggered)
			continue;
		Results.push_back(this->Invoke(Descriptor->ID, Context));
	}
}

EditorActionResult EditorActionRegistry::Invoke(const EditorActionID ID, EditorActionContext &Context)
{
	const EditorActionDescriptor *Descriptor = this->Find(ID);
	if (Descriptor == nullptr)
		throw std::out_of_range("Unknown editor action identity");
	if (Descriptor->CanExecute && !Descriptor->CanExecute(Context))
		return {.ID = ID, .Status = EditorActionStatus::Disabled, .Diagnostic = this->GetDisabledReason(ID, Context)};
	try
	{
		Descriptor->Execute(Context);
		return {.ID = ID, .Status = EditorActionStatus::Executed};
	}
	catch (const std::exception &Exception)
	{
		const string Diagnostic = "Action '" + Descriptor->DisplayName + "' failed: " + Exception.what();
		Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "EditorAction", Diagnostic);
		return {.ID = ID, .Status = EditorActionStatus::Failed, .Diagnostic = Diagnostic};
	}
	catch (...)
	{
		const string Diagnostic = "Action '" + Descriptor->DisplayName + "' failed with a non-standard exception";
		Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "EditorAction", Diagnostic);
		return {.ID = ID, .Status = EditorActionStatus::Failed, .Diagnostic = Diagnostic};
	}
}

const EditorActionDescriptor *EditorActionRegistry::Find(const EditorActionID ID) const noexcept
{
	const auto Action = this->Actions.find(ID);
	return Action == this->Actions.end() ? nullptr : &Action->second;
}

void EditorActionRegistry::SnapshotInto(std::vector<const EditorActionDescriptor *> &Result) const
{
	Result.clear();
	Result.reserve(this->Actions.size());
	for (const auto &[ID, Descriptor] : this->Actions)
	{
		(void)ID;
		Result.push_back(&Descriptor);
	}
	std::ranges::sort(Result,
					  [](const EditorActionDescriptor *Left, const EditorActionDescriptor *Right)
					  {
						  if (Left->Category != Right->Category)
							  return Left->Category < Right->Category;
						  if (Left->Order != Right->Order)
							  return Left->Order < Right->Order;
						  return Left->DisplayName < Right->DisplayName;
					  });
}
std::vector<const EditorActionDescriptor *> EditorActionRegistry::Snapshot() const
{
	std::vector<const EditorActionDescriptor *> Result;
	this->SnapshotInto(Result);
	return Result;
}

bool EditorActionRegistry::CanExecute(const EditorActionID ID, const EditorActionContext &Context) const
{
	const EditorActionDescriptor *Descriptor = this->Find(ID);
	if (Descriptor == nullptr)
		throw std::out_of_range("Unknown editor action identity");
	return !Descriptor->CanExecute || Descriptor->CanExecute(Context);
}

string EditorActionRegistry::GetDisabledReason(const EditorActionID ID, const EditorActionContext &Context) const
{
	const EditorActionDescriptor *Descriptor = this->Find(ID);
	if (Descriptor == nullptr)
		throw std::out_of_range("Unknown editor action identity");
	if (!Descriptor->CanExecute || Descriptor->CanExecute(Context))
		return {};
	if (Descriptor->DisabledReason)
	{
		string Reason = Descriptor->DisabledReason(Context);
		if (!Reason.empty())
			return Reason;
	}
	return "'" + Descriptor->DisplayName + "' is unavailable because its current editor-state requirements are not satisfied.";
}

bool EditorActionRegistry::IsChecked(const EditorActionID ID, const EditorActionContext &Context) const
{
	const EditorActionDescriptor *Descriptor = this->Find(ID);
	if (Descriptor == nullptr)
		throw std::out_of_range("Unknown editor action identity");
	return Descriptor->IsChecked && Descriptor->IsChecked(Context);
}

void RegisterCoreEditorActions(EditorActionRegistry &Registry)
{
	Registry.Register({.ID = IDs::OpenScene,
					   .Name = "File.OpenScene",
					   .DisplayName = "Open",
					   .Description = "Open an engine scene document",
					   .Icon = "Open",
					   .Category = EditorActionCategory::File,
					   .Order = 10,
					   .Shortcut = KeyBinding(IDs::OpenScene, core::input::Key::O, core::input::Modifier::Control),
					   .CanExecute = [](const EditorActionContext &Context)
					   { return Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped; },
					   .Execute =
						   [](EditorActionContext &Context)
					   {
						   if (Context.Session.GetDocument().IsDirty())
						   {
							   if (Context.Window == nullptr)
								   throw std::logic_error("Unsaved-scene confirmation requires an application window");
							   const core::DialogResult<core::TaskDialogSelection> Choice = Context.Window->ShowTaskDialog(
								   {.Title = "Unsaved Scene",
									.Instruction = "Save changes before opening another scene?",
									.Content = "Opening another scene will replace the current editor document.",
									.Buttons = {{.ID = 100, .Text = "Save", .IsDefault = true},
												{.ID = 101, .Text = "Discard"},
												{.ID = 102, .Text = "Cancel"}},
									.Severity = core::TaskDialogSeverity::Warning});
							   if (!Choice.Accepted() || Choice.Value->ButtonID == 102)
								   return;
							   if (Choice.Value->ButtonID == 100 && !SaveScene(Context, false))
								   return;
						   }
						   const std::optional<std::filesystem::path> Path = SelectScenePath(Context, core::FileDialogOperation::OpenFile);
						   if (Path.has_value())
							   Context.Session.OpenDocument(*Path);
					   }});
	Registry.Register({.ID = IDs::SaveScene,
					   .Name = "File.SaveScene",
					   .DisplayName = "Save",
					   .Description = "Save the current engine scene document",
					   .Icon = "Save",
					   .Category = EditorActionCategory::File,
					   .Order = 20,
					   .Shortcut = KeyBinding(IDs::SaveScene, core::input::Key::S, core::input::Modifier::Control),
					   .CanExecute = [](const EditorActionContext &Context)
					   { return Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped; },
					   .Execute = [](EditorActionContext &Context) { (void)SaveScene(Context, false); }});
	Registry.Register({.ID = IDs::CookProject,
					   .Name = "Build.Cook",
					   .DisplayName = "Cook",
					   .Description = "Cook project content into versioned compressed archives",
					   .Icon = "Cook",
					   .Category = EditorActionCategory::Build,
					   .Order = 10,
					   .CanExecute =
						   [](const EditorActionContext &Context)
					   {
						   const cook::CookPackageState State = Context.Session.GetCookPackageService().GetState();
						   return State != cook::CookPackageState::Cooking && State != cook::CookPackageState::Publishing;
					   },
					   .Execute = [](EditorActionContext &Context) { Context.Session.BeginCook(Context.Scheduler); }});
	Registry.Register({.ID = IDs::BuildGameModule,
					   .Name = "Build.GameModule",
					   .DisplayName = "Build GameModule",
					   .Description = "Compile and atomically publish the active project's GameModule",
					   .Icon = "Cook",
					   .Category = EditorActionCategory::Build,
					   .Order = 5,
					   .CanExecute =
						   [](const EditorActionContext &Context)
					   {
						   const build::ProjectBuildService &Build = Context.Session.GetProjectBuildService();
						   return Build.CanBuildGameModule() && Build.GetState() != build::ProjectBuildState::Building;
					   },
					   .Execute = [](EditorActionContext &Context) { Context.Session.BeginGameModuleBuild(Context.Scheduler); }});
	Registry.Register({.ID = IDs::CancelProjectBuild,
					   .Name = "Build.Cancel",
					   .DisplayName = "Cancel Build",
					   .Description = "Cancel the active project build and its child processes",
					   .Icon = "Stop",
					   .Category = EditorActionCategory::Build,
					   .Order = 6,
					   .CanExecute =
						   [](const EditorActionContext &Context)
					   {
						   const cook::CookPackageState CookState = Context.Session.GetCookPackageService().GetState();
						   return Context.Session.GetProjectBuildService().GetState() == build::ProjectBuildState::Building ||
								  CookState == cook::CookPackageState::Cooking || CookState == cook::CookPackageState::Publishing;
					   },
					   .Execute =
						   [](EditorActionContext &Context)
					   {
						   Context.Session.GetProjectBuildService().Cancel();
						   Context.Session.GetCookPackageService().Cancel();
					   }});
	Registry.Register({.ID = IDs::PackageProject,
					   .Name = "Build.Package",
					   .DisplayName = "Package",
					   .Description = "Cook and publish a runnable package from the bundled OpenFrame runtime",
					   .Icon = "Package",
					   .Category = EditorActionCategory::Build,
					   .Order = 20,
					   .CanExecute =
						   [](const EditorActionContext &Context)
					   {
						   const cook::CookPackageState State = Context.Session.GetCookPackageService().GetState();
						   return Context.Session.HasRuntimePackageFiles() && State != cook::CookPackageState::Cooking &&
								  State != cook::CookPackageState::Publishing;
					   },
					   .Execute = [](EditorActionContext &Context) { Context.Session.BeginPackage(Context.Scheduler); }});
	Registry.Register(
		{.ID = IDs::Undo,
		 .Name = "Edit.Undo",
		 .DisplayName = "Undo",
		 .Description = "Undo the last editor transaction",
		 .Icon = "Undo",
		 .Category = EditorActionCategory::Edit,
		 .Order = 10,
		 .Shortcut = KeyBinding(IDs::Undo, core::input::Key::Z, core::input::Modifier::Control),
		 .CanExecute = [](const EditorActionContext &Context)
		 { return EditWorldAvailable(Context) && Context.Session.GetDocument().GetHistory().CanUndo(); },
		 .DisabledReason = [](const EditorActionContext &Context)
		 { return EditWorldAvailable(Context) ? "There is no editor transaction to undo." : EditWorldDisabledReason(Context); },
		 .Execute = [](EditorActionContext &Context) { Context.Session.GetDocument().Undo(); }});
	Registry.Register(
		{.ID = IDs::Redo,
		 .Name = "Edit.Redo",
		 .DisplayName = "Redo",
		 .Description = "Redo the next editor transaction",
		 .Icon = "Redo",
		 .Category = EditorActionCategory::Edit,
		 .Order = 20,
		 .Shortcut = KeyBinding(IDs::Redo, core::input::Key::Y, core::input::Modifier::Control),
		 .CanExecute = [](const EditorActionContext &Context)
		 { return EditWorldAvailable(Context) && Context.Session.GetDocument().GetHistory().CanRedo(); },
		 .DisabledReason = [](const EditorActionContext &Context)
		 { return EditWorldAvailable(Context) ? "There is no editor transaction to redo." : EditWorldDisabledReason(Context); },
		 .Execute = [](EditorActionContext &Context) { Context.Session.GetDocument().Redo(); }});
	Registry.Register({.ID = IDs::CopyObjects,
					   .Name = "Edit.CopyObjects",
					   .DisplayName = "Copy",
					   .Description = "Copy complete selected scene-object subtrees into the editor clipboard",
					   .Icon = "Duplicate",
					   .Category = EditorActionCategory::Edit,
					   .Order = 30,
					   .Shortcut = KeyBinding(IDs::CopyObjects, core::input::Key::C, core::input::Modifier::Control),
					   .CanExecute = [](const EditorActionContext &Context)
					   { return EditWorldAvailable(Context) && !Context.Session.GetDocument().GetSelection().Empty(); },
					   .DisabledReason = &SelectionActionDisabledReason,
					   .Execute = [](EditorActionContext &Context) { Context.Session.CopySelection(); }});
	Registry.Register(
		{.ID = IDs::PasteObjects,
		 .Name = "Edit.PasteObjects",
		 .DisplayName = "Paste",
		 .Description = "Paste a new undoable copy of the scene-object clipboard",
		 .Icon = "Duplicate",
		 .Category = EditorActionCategory::Edit,
		 .Order = 40,
		 .Shortcut = KeyBinding(IDs::PasteObjects, core::input::Key::V, core::input::Modifier::Control),
		 .CanExecute = [](const EditorActionContext &Context)
		 { return EditWorldAvailable(Context) && Context.Session.CanPasteClipboard(); },
		 .DisabledReason = [](const EditorActionContext &Context)
		 { return EditWorldAvailable(Context) ? "Copy one or more scene objects before pasting." : EditWorldDisabledReason(Context); },
		 .Execute =
			 [](EditorActionContext &Context)
		 {
			 Context.Session.PasteClipboard();
			 Context.Session.CloneSelectedPrivateMaterials(Context.Scheduler);
		 }});
	Registry.Register({.ID = IDs::DuplicateObjects,
					   .Name = "Edit.DuplicateObjects",
					   .DisplayName = "Duplicate",
					   .Description = "Duplicate the selected scene-object subtrees",
					   .Icon = "Duplicate",
					   .Category = EditorActionCategory::Edit,
					   .Order = 50,
					   .Shortcut = KeyBinding(IDs::DuplicateObjects, core::input::Key::D, core::input::Modifier::Control),
					   .CanExecute = [](const EditorActionContext &Context)
					   { return EditWorldAvailable(Context) && !Context.Session.GetDocument().GetSelection().Empty(); },
					   .DisabledReason = &SelectionActionDisabledReason,
					   .Execute =
						   [](EditorActionContext &Context)
					   {
						   Context.Session.GetDocument().Execute(std::make_unique<commands::DuplicateObjectsCommand>(
							   Context.Session.GetDocument(), Context.Session.GetDocument().GetSelection().GetOrdered()));
						   Context.Session.CloneSelectedPrivateMaterials(Context.Scheduler);
					   }});
	Registry.Register({.ID = IDs::GroupObjects,
					   .Name = "Edit.GroupObjects",
					   .DisplayName = "Group",
					   .Description = "Create a transformable hierarchy group around the selected scene-object roots",
					   .Icon = "Group",
					   .Category = EditorActionCategory::Edit,
					   .Order = 55,
					   .Shortcut = KeyBinding(IDs::GroupObjects, core::input::Key::G, core::input::Modifier::Control),
					   .CanExecute = [](const EditorActionContext &Context)
					   { return EditWorldAvailable(Context) && !Context.Session.GetDocument().GetSelection().Empty(); },
					   .DisabledReason = &SelectionActionDisabledReason,
					   .Execute = [](EditorActionContext &Context) { Context.Session.GroupSelection(); }});
	Registry.Register({.ID = IDs::ToggleSelectionLocked,
					   .Name = "Edit.ToggleSelectionLocked",
					   .DisplayName = "Lock",
					   .Description = "Lock or unlock the selected objects for editor manipulation",
					   .Icon = "Lock",
					   .Category = EditorActionCategory::Edit,
					   .Order = 56,
					   .CanExecute = [](const EditorActionContext &Context)
					   { return EditWorldAvailable(Context) && !Context.Session.GetDocument().GetSelection().Empty(); },
					   .DisabledReason = &SelectionActionDisabledReason,
					   .IsChecked = [](const EditorActionContext &Context) { return GetPrimaryLocked(Context).value_or(false); },
					   .Execute = [](EditorActionContext &Context)
					   { EditSelectedIdentityProperty(Context, "Locked", !GetPrimaryLocked(Context).value_or(false)); }});
	const auto RegisterMobility =
		[&Registry](const EditorActionID ID, string Name, string DisplayName, const components::ObjectMobility Mobility, const int32 Order)
	{
		Registry.Register({.ID = ID,
						   .Name = std::move(Name),
						   .DisplayName = std::move(DisplayName),
						   .Description = "Set mobility for the selected objects",
						   .Icon = "Mobility",
						   .Category = EditorActionCategory::Edit,
						   .Order = Order,
						   .CanExecute = [](const EditorActionContext &Context)
						   { return EditWorldAvailable(Context) && !Context.Session.GetDocument().GetSelection().Empty(); },
						   .DisabledReason = &SelectionActionDisabledReason,
						   .IsChecked = [Mobility](const EditorActionContext &Context) { return GetPrimaryMobility(Context) == Mobility; },
						   .Execute = [Mobility](EditorActionContext &Context)
						   { EditSelectedIdentityProperty(Context, "Mobility", static_cast<uint32>(Mobility)); }});
	};
	RegisterMobility(IDs::MobilityStatic, "Edit.Mobility.Static", "Static", components::ObjectMobility::Static, 57);
	RegisterMobility(IDs::MobilityStationary, "Edit.Mobility.Stationary", "Stationary", components::ObjectMobility::Stationary, 58);
	RegisterMobility(IDs::MobilityMovable, "Edit.Mobility.Movable", "Movable", components::ObjectMobility::Movable, 59);
	Registry.Register({.ID = IDs::DeleteObjects,
					   .Name = "Edit.DeleteObjects",
					   .DisplayName = "Delete",
					   .Description = "Delete the selected scene-object subtrees",
					   .Icon = "Delete",
					   .Category = EditorActionCategory::Edit,
					   .Order = 60,
					   .Shortcut = KeyBinding(IDs::DeleteObjects, core::input::Key::Delete),
					   .CanExecute = [](const EditorActionContext &Context)
					   { return EditWorldAvailable(Context) && !Context.Session.GetDocument().GetSelection().Empty(); },
					   .DisabledReason = &SelectionActionDisabledReason,
					   .Execute = [](EditorActionContext &Context) { Context.Session.DeleteSelectedObjects(Context.Scheduler); }});
	Registry.Register(
		{.ID = IDs::ImportAssets,
		 .Name = "Content.Import",
		 .DisplayName = "Import",
		 .Description = "Import source files into the project Content directory",
		 .Icon = "Import",
		 .Category = EditorActionCategory::Assets,
		 .Order = 30,
		 .CanExecute = [](const EditorActionContext &Context)
		 { return Context.Window != nullptr && !Context.Session.GetAssetImportService().IsBusy(); },
		 .Execute = [](EditorActionContext &Context) { Context.Session.GetAssetImportService().BeginFileSelection(*Context.Window); }});
	Registry.Register({.ID = IDs::CancelAssetImport,
					   .Name = "Content.CancelImport",
					   .DisplayName = "Cancel Import",
					   .Description = "Cancel the active asset import before publication",
					   .Icon = "Stop",
					   .Category = EditorActionCategory::Assets,
					   .Order = 31,
					   .CanExecute = [](const EditorActionContext &Context)
					   { return Context.Session.GetAssetImportService().GetState() == asset::AssetImportServiceState::Importing; },
					   .Execute = [](EditorActionContext &Context) { Context.Session.GetAssetImportService().Cancel(); }});

	const auto RegisterPrimitive = [&Registry](const EditorActionID ID, string Name, string DisplayName, string Icon,
											   const asset::PrimitiveShape Shape, const int32 Order)
	{
		Registry.Register({.ID = ID,
						   .Name = std::move(Name),
						   .DisplayName = std::move(DisplayName),
						   .Description = "Create a renderable primitive in the current scene",
						   .Icon = std::move(Icon),
						   .Category = EditorActionCategory::Assets,
						   .Order = Order,
						   .CanExecute = &EditWorldAvailable,
						   .DisabledReason = &EditWorldDisabledReason,
						   .Execute = [Shape](EditorActionContext &Context) { Context.Session.CreatePrimitive(Shape); }});
	};
	RegisterPrimitive(IDs::CreateBox, "Create.Box", "Box", "Box", asset::PrimitiveShape::Box, 40);
	RegisterPrimitive(IDs::CreateSphere, "Create.Sphere", "Sphere", "Sphere", asset::PrimitiveShape::Sphere, 41);
	RegisterPrimitive(IDs::CreateCapsule, "Create.Capsule", "Capsule", "Capsule", asset::PrimitiveShape::Capsule, 42);
	RegisterPrimitive(IDs::CreateCylinder, "Create.Cylinder", "Cylinder", "Cylinder", asset::PrimitiveShape::Cylinder, 43);
	RegisterPrimitive(IDs::CreateCone, "Create.Cone", "Cone", "Cone", asset::PrimitiveShape::Cone, 44);
	RegisterPrimitive(IDs::CreatePlane, "Create.Plane", "Plane", "Plane", asset::PrimitiveShape::Plane, 45);

	const auto RegisterTool = [&Registry](const EditorActionID ID, string Name, string DisplayName, string Icon,
										  const core::input::Key Shortcut, const viewport::TransformGizmoOperation Operation,
										  const int32 Order)
	{
		Registry.Register(
			{.ID = ID,
			 .Name = std::move(Name),
			 .DisplayName = std::move(DisplayName),
			 .Description = "Activate the viewport transform tool",
			 .Icon = std::move(Icon),
			 .Category = EditorActionCategory::Transform,
			 .Order = Order,
			 .Shortcut = KeyBinding(ID, Shortcut),
			 .CanExecute = &EditWorldAvailable,
			 .DisabledReason = &EditWorldDisabledReason,
			 .IsChecked = [Operation](const EditorActionContext &Context)
			 { return Context.Session.GetTransformGizmo().GetOperation() == Operation; },
			 .Execute = [Operation](EditorActionContext &Context) { Context.Session.GetTransformGizmo().SetOperation(Operation); }});
	};
	RegisterTool(IDs::SelectTool, "Transform.Select", "Select", "Cursor", core::input::Key::Q, viewport::TransformGizmoOperation::Select,
				 10);
	RegisterTool(IDs::TranslateTool, "Transform.Translate", "Move", "Move", core::input::Key::W,
				 viewport::TransformGizmoOperation::Translate, 20);
	RegisterTool(IDs::RotateTool, "Transform.Rotate", "Rotate", "Rotate", core::input::Key::E, viewport::TransformGizmoOperation::Rotate,
				 30);
	RegisterTool(IDs::ScaleTool, "Transform.Scale", "Scale", "Scale", core::input::Key::R, viewport::TransformGizmoOperation::Scale, 40);
	RegisterTool(IDs::UniversalTool, "Transform.Universal", "Universal", "Axes", core::input::Key::T,
				 viewport::TransformGizmoOperation::Universal, 45);
	Registry.Register({.ID = IDs::ToggleTransformSpace,
					   .Name = "Transform.ToggleSpace",
					   .DisplayName = "Local/World",
					   .Description = "Toggle transform axes between local and world space",
					   .Icon = "Axes",
					   .Category = EditorActionCategory::Transform,
					   .Order = 50,
					   .Shortcut = KeyBinding(IDs::ToggleTransformSpace, core::input::Key::X),
					   .CanExecute = &EditWorldAvailable,
					   .IsChecked = [](const EditorActionContext &Context)
					   { return Context.Session.GetTransformGizmo().GetSpace() == viewport::TransformGizmoSpace::Local; },
					   .Execute =
						   [](EditorActionContext &Context)
					   {
						   viewport::TransformGizmoController &Gizmo = Context.Session.GetTransformGizmo();
						   Gizmo.SetSpace(Gizmo.GetSpace() == viewport::TransformGizmoSpace::Local ? viewport::TransformGizmoSpace::World
																								   : viewport::TransformGizmoSpace::Local);
					   }});
	Registry.Register(
		{.ID = IDs::ToggleTransformSnapping,
		 .Name = "Transform.ToggleSnapping",
		 .DisplayName = "Snapping",
		 .Description = "Toggle transform snapping",
		 .Icon = "Magnet",
		 .Category = EditorActionCategory::Transform,
		 .Order = 60,
		 .Shortcut = KeyBinding(IDs::ToggleTransformSnapping, core::input::Key::S, core::input::Modifier::Shift),
		 .CanExecute = &EditWorldAvailable,
		 .IsChecked = [](const EditorActionContext &Context) { return Context.Session.GetTransformGizmo().GetSnapSettings().Enabled; },
		 .Execute =
			 [](EditorActionContext &Context)
		 {
			 preferences::EditorPreferences Preferences = Context.Session.GetPreferences();
			 Preferences.TransformSnappingEnabled = !Context.Session.GetTransformGizmo().GetSnapSettings().Enabled;
			 Context.Session.SetPreferences(std::move(Preferences));
		 }});
	const auto RegisterPivot =
		[&Registry](const EditorActionID ID, string Name, string DisplayName, const viewport::TransformGizmoPivot Pivot, const int32 Order)
	{
		Registry.Register(
			{.ID = ID,
			 .Name = std::move(Name),
			 .DisplayName = std::move(DisplayName),
			 .Description = "Use this pivot mode for viewport transform operations",
			 .Icon = "Axes",
			 .Category = EditorActionCategory::Transform,
			 .Order = Order,
			 .CanExecute = &EditWorldAvailable,
			 .DisabledReason = &EditWorldDisabledReason,
			 .IsChecked = [Pivot](const EditorActionContext &Context) { return Context.Session.GetTransformGizmo().GetPivot() == Pivot; },
			 .Execute = [Pivot](EditorActionContext &Context) { Context.Session.GetTransformGizmo().SetPivot(Pivot); }});
	};
	RegisterPivot(IDs::PivotActiveObject, "Transform.Pivot.ActiveObject", "Pivot: Active Object",
				  viewport::TransformGizmoPivot::ActiveObject, 70);
	RegisterPivot(IDs::PivotMedianPoint, "Transform.Pivot.MedianPoint", "Pivot: Median Point", viewport::TransformGizmoPivot::MedianPoint,
				  71);
	RegisterPivot(IDs::PivotIndividualOrigins, "Transform.Pivot.IndividualOrigins", "Pivot: Individual Origins",
				  viewport::TransformGizmoPivot::IndividualOrigins, 72);
	RegisterPivot(IDs::PivotBoundingBoxCenter, "Transform.Pivot.BoundingBoxCenter", "Pivot: Bounding Box Center",
				  viewport::TransformGizmoPivot::BoundingBoxCenter, 73);
	RegisterPivot(IDs::PivotWorldOrigin, "Transform.Pivot.WorldOrigin", "Pivot: World Origin", viewport::TransformGizmoPivot::WorldOrigin,
				  74);
	Registry.Register({.ID = IDs::CycleTransformPivot,
					   .Name = "Transform.Pivot.Cycle",
					   .DisplayName = "Pivot",
					   .Description = "Cycle through active, median, individual, bounding-box, and world-origin pivots",
					   .Icon = "Axes",
					   .Category = EditorActionCategory::Transform,
					   .Order = 75,
					   .CanExecute = &EditWorldAvailable,
					   .Execute = [](EditorActionContext &Context)
					   {
						   viewport::TransformGizmoController &Gizmo = Context.Session.GetTransformGizmo();
						   const uint8 Next =
							   (static_cast<uint8>(Gizmo.GetPivot()) + 1U) % static_cast<uint8>(viewport::TransformGizmoPivot::Count);
						   Gizmo.SetPivot(static_cast<viewport::TransformGizmoPivot>(Next));
					   }});

	Registry.Register({.ID = IDs::Play,
					   .Name = "Play.Toggle",
					   .DisplayName = "Play/Stop",
					   .Description = "Start an isolated play world or stop the current session",
					   .Icon = "Play",
					   .Category = EditorActionCategory::Test,
					   .Order = 10,
					   .Shortcut = KeyBinding(IDs::Play, core::input::Key::F5),
					   .Execute = [](EditorActionContext &Context)
					   {
						   if (Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped)
							   Context.Session.StartPlay(Context.Scheduler);
						   else
							   Context.Session.StopPlay();
					   }});
	Registry.Register({.ID = IDs::Simulate,
					   .Name = "Play.Simulate",
					   .DisplayName = "Simulate/Stop",
					   .Description = "Run an isolated editor-controlled simulation without game behavior callbacks",
					   .Icon = "Play",
					   .Category = EditorActionCategory::Test,
					   .Order = 15,
					   .Shortcut = KeyBinding(IDs::Simulate, core::input::Key::F8),
					   .Execute = [](EditorActionContext &Context)
					   {
						   if (Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped)
							   Context.Session.StartSimulate();
						   else
							   Context.Session.StopPlay();
					   }});
	Registry.Register({.ID = IDs::Pause,
					   .Name = "Play.Pause",
					   .DisplayName = "Pause/Resume",
					   .Description = "Pause or resume the isolated play world",
					   .Icon = "Pause",
					   .Category = EditorActionCategory::Test,
					   .Order = 20,
					   .Shortcut = KeyBinding(IDs::Pause, core::input::Key::F6),
					   .CanExecute =
						   [](const EditorActionContext &Context)
					   {
						   const play::PlaySessionState State = Context.Session.GetPlaySession().GetState();
						   return State == play::PlaySessionState::Playing || State == play::PlaySessionState::Paused;
					   },
					   .IsChecked = [](const EditorActionContext &Context)
					   { return Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Paused; },
					   .Execute =
						   [](EditorActionContext &Context)
					   {
						   if (Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Playing)
							   Context.Session.PausePlay();
						   else
							   Context.Session.ResumePlay();
					   }});
	Registry.Register({.ID = IDs::Step,
					   .Name = "Play.Step",
					   .DisplayName = "Step",
					   .Description = "Advance a paused play world by one fixed simulation step",
					   .Icon = "Step",
					   .Category = EditorActionCategory::Test,
					   .Order = 30,
					   .Shortcut = KeyBinding(IDs::Step, core::input::Key::F7),
					   .CanExecute = [](const EditorActionContext &Context)
					   { return Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Paused; },
					   .Execute = [](EditorActionContext &Context) { Context.Session.StepPlay(Context.Scheduler); }});
	Registry.Register({.ID = IDs::Standalone,
					   .Name = "Play.Standalone",
					   .DisplayName = "Standalone",
					   .Description = "Save and launch the selected scene in the standalone Game executable",
					   .Icon = "Play",
					   .Category = EditorActionCategory::Test,
					   .Order = 40,
					   .CanExecute =
						   [](const EditorActionContext &Context)
					   {
						   return Context.Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped &&
								  !Context.Session.GetDocument().GetPath().empty() && Context.Session.HasRuntimePackageFiles();
					   },
					   .Execute =
						   [](EditorActionContext &Context)
					   {
						   const uint32 ProcessID = Context.Session.LaunchStandalone();
						   Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Information, "Standalone",
													   "Launched standalone game process " + std::to_string(ProcessID));
					   }});

	const auto RegisterPanel =
		[&Registry](const EditorActionID ID, string Name, string DisplayName, const workspace::EditorPanelID Panel, const int32 Order)
	{
		Registry.Register(
			{.ID = ID,
			 .Name = std::move(Name),
			 .DisplayName = std::move(DisplayName),
			 .Description = "Show or hide an editor workspace panel",
			 .Icon = "Panel",
			 .Category = EditorActionCategory::View,
			 .Order = Order,
			 .IsChecked = [Panel](const EditorActionContext &Context) { return Context.Session.GetWorkspace().GetPanel(Panel).Open; },
			 .Execute = [Panel](EditorActionContext &Context) { Context.Session.GetWorkspace().Toggle(Panel); }});
	};
	RegisterPanel(IDs::ToggleProperties, "View.Properties", "Properties", workspace::EditorPanelID::Properties, 10);
	RegisterPanel(IDs::ToggleExplorer, "View.Explorer", "Explorer", workspace::EditorPanelID::Explorer, 20);
	RegisterPanel(IDs::ToggleAssetBrowser, "View.Assets", "Assets", workspace::EditorPanelID::AssetBrowser, 30);
	RegisterPanel(IDs::ToggleMaterialEditor, "View.MaterialEditor", "Material Editor", workspace::EditorPanelID::MaterialEditor, 40);
	RegisterPanel(IDs::ToggleOutput, "View.Output", "Output", workspace::EditorPanelID::Output, 50);
	RegisterPanel(IDs::ToggleDiagnostics, "View.Diagnostics", "Diagnostics", workspace::EditorPanelID::Diagnostics, 60);
	Registry.Register({.ID = IDs::ShowCommandReference,
					   .Name = "Help.CommandReference",
					   .DisplayName = "Command Reference",
					   .Description = "Browse every registered editor command and its purpose",
					   .Icon = "Help",
					   .Category = EditorActionCategory::Help,
					   .Order = 10,
					   .Execute = [](EditorActionContext &Context) { Context.Session.SetCommandReferenceOpen(true); }});
	Registry.Register({.ID = IDs::ShowAbout,
					   .Name = "Help.About",
					   .DisplayName = "About",
					   .Description = "Show engine, renderer, and project information",
					   .Icon = "Information",
					   .Category = EditorActionCategory::Help,
					   .Order = 20,
					   .Execute = [](EditorActionContext &Context) { Context.Session.SetAboutOpen(true); }});
	Registry.Register(
		{.ID = IDs::EditMaterialColor,
		 .Name = "Assets.EditMaterialColor",
		 .DisplayName = "Color",
		 .Description = "Edit the selected mesh material slot's private base color",
		 .Icon = "Color",
		 .Category = EditorActionCategory::Assets,
		 .Order = 35,
		 .CanExecute = [](const EditorActionContext &Context) { return EditWorldAvailable(Context) && HasSelectedMesh(Context); },
		 .DisabledReason =
			 [](const EditorActionContext &Context)
		 {
			 return EditWorldAvailable(Context) ? "Select a scene object with a mesh component before editing its material color."
												: EditWorldDisabledReason(Context);
		 },
		 .Execute = [](EditorActionContext &Context) { Context.Session.RequestMaterialColorFocus(); }});

	const auto RegisterViewMode = [&Registry](const EditorActionID ID, string Name, string DisplayName,
											  const pipeline::render::ViewportViewMode Mode, const int32 Order)
	{
		Registry.Register({.ID = ID,
						   .Name = std::move(Name),
						   .DisplayName = std::move(DisplayName),
						   .Description = "Set the active viewport visualization mode",
						   .Icon = "ViewMode",
						   .Category = EditorActionCategory::View,
						   .Order = Order,
						   .CanExecute = [](const EditorActionContext &Context) { return Context.ActiveViewportSettings != nullptr; },
						   .DisabledReason = &ActiveViewportDisabledReason,
						   .IsChecked = [Mode](const EditorActionContext &Context)
						   { return Context.ActiveViewportSettings != nullptr && Context.ActiveViewportSettings->ViewMode == Mode; },
						   .Execute = [Mode](EditorActionContext &Context) { Context.ActiveViewportSettings->ViewMode = Mode; }});
	};
	RegisterViewMode(IDs::ViewLit, "View.Mode.Lit", "Lit", pipeline::render::ViewportViewMode::Lit, 100);
	RegisterViewMode(IDs::ViewUnlit, "View.Mode.Unlit", "Unlit", pipeline::render::ViewportViewMode::Unlit, 101);
	RegisterViewMode(IDs::ViewWireframe, "View.Mode.Wireframe", "Wireframe", pipeline::render::ViewportViewMode::Wireframe, 102);
	RegisterViewMode(IDs::ViewNormals, "View.Mode.Normals", "Normals", pipeline::render::ViewportViewMode::WorldNormals, 103);
	RegisterViewMode(IDs::ViewDepth, "View.Mode.Depth", "Depth", pipeline::render::ViewportViewMode::LinearDepth, 104);
	RegisterViewMode(IDs::ViewObjectID, "View.Mode.ObjectID", "Object ID", pipeline::render::ViewportViewMode::ObjectID, 105);
	RegisterViewMode(IDs::ViewOverdraw, "View.Mode.Overdraw", "Overdraw", pipeline::render::ViewportViewMode::Overdraw, 106);

	const auto RegisterOverlay = [&Registry](const EditorActionID ID, string Name, string DisplayName,
											 bool pipeline::render::ViewportOverlaySettings::*Member, const int32 Order)
	{
		Registry.Register({.ID = ID,
						   .Name = std::move(Name),
						   .DisplayName = std::move(DisplayName),
						   .Description = "Toggle an overlay in the active viewport",
						   .Icon = "Overlay",
						   .Category = EditorActionCategory::View,
						   .Order = Order,
						   .CanExecute = [](const EditorActionContext &Context) { return Context.ActiveViewportSettings != nullptr; },
						   .DisabledReason = &ActiveViewportDisabledReason,
						   .IsChecked = [Member](const EditorActionContext &Context)
						   { return Context.ActiveViewportSettings != nullptr && Context.ActiveViewportSettings->Overlays.*Member; },
						   .Execute =
							   [Member](EditorActionContext &Context)
						   {
							   bool &Value = Context.ActiveViewportSettings->Overlays.*Member;
							   Value = !Value;
						   }});
	};
	RegisterOverlay(IDs::OverlayGrid, "View.Overlay.Grid", "Grid", &pipeline::render::ViewportOverlaySettings::Grid, 120);
	RegisterOverlay(IDs::OverlayBounds, "View.Overlay.Bounds", "Bounds", &pipeline::render::ViewportOverlaySettings::Bounds, 121);
	RegisterOverlay(IDs::OverlaySkeletons, "View.Overlay.Skeletons", "Skeletons", &pipeline::render::ViewportOverlaySettings::Skeletons,
					122);
	RegisterOverlay(IDs::OverlayCameras, "View.Overlay.Cameras", "Cameras", &pipeline::render::ViewportOverlaySettings::Cameras, 123);
	RegisterOverlay(IDs::OverlayLights, "View.Overlay.Lights", "Lights", &pipeline::render::ViewportOverlaySettings::Lights, 124);
	RegisterOverlay(IDs::OverlayCulling, "View.Overlay.Culling", "Culling", &pipeline::render::ViewportOverlaySettings::Culling, 125);
	RegisterOverlay(IDs::OverlaySelection, "View.Overlay.Selection", "Selection", &pipeline::render::ViewportOverlaySettings::Selection,
					126);
	RegisterOverlay(IDs::OverlayRenderStatistics, "View.Overlay.RenderStatistics", "Render Statistics",
					&pipeline::render::ViewportOverlaySettings::RenderStatistics, 127);
	RegisterOverlay(IDs::OverlayRenderGraph, "View.Overlay.RenderGraph", "Render Graph",
					&pipeline::render::ViewportOverlaySettings::RenderGraph, 128);
	Registry.Register({.ID = IDs::ResetWorkspace,
					   .Name = "Window.ResetWorkspace",
					   .DisplayName = "Reset Layout",
					   .Description = "Restore the reference editor panel layout",
					   .Icon = "Panel",
					   .Category = EditorActionCategory::Window,
					   .Order = 10,
					   .Execute = [](EditorActionContext &Context) { Context.Session.GetWorkspace().ResetToReferenceLayout(); }});
	Registry.Register({.ID = IDs::OpenPreferences,
					   .Name = "Window.Preferences",
					   .DisplayName = "Preferences",
					   .Description = "Configure project editor behavior",
					   .Icon = "Settings",
					   .Category = EditorActionCategory::Window,
					   .Order = 20,
					   .Execute = [](EditorActionContext &Context) { Context.Session.SetPreferencesOpen(true); }});
}
} // namespace editor::action
