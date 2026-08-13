#include "EditorUserInterface.h"

#include "EditorDockspace.h"
#include "EditorIconRegistry.h"
#include "EditorLayoutStore.h"
#include "EditorUIContext.h"
#include "EditorUIRenderer.h"
#include "EditorUIWindowBridge.h"
#include "Source/component/object/CObjectAnimationComponent.h"
#include "Source/component/object/CObjectBehaviorComponent.h"
#include "Source/component/object/CObjectCameraComponent.h"
#include "Source/component/object/CObjectHierarchyComponent.h"
#include "Source/component/object/CObjectIdentityComponent.h"
#include "Source/component/object/CObjectLightComponents.h"
#include "Source/component/object/CObjectMeshComponent.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/core/app/ApplicationServices.h"
#include "Source/core/EngineVersion.h"
#include "Source/core/diagnostics/DiagnosticSink.h"
#include "Source/core/input/InputSystem.h"
#include "Source/core/window/Context.h"
#include "Source/core/window/Window.h"
#ifdef CreateWindow
#undef CreateWindow
#endif
#include "Source/core/window/WindowManager.h"
#include "Source/editor/EditorSession.h"
#include "Source/editor/action/EditorActionRegistry.h"
#include "Source/editor/commands/PropertyEditCommand.h"
#include "Source/editor/commands/MeshMaterialOverrideCommand.h"
#include "Source/editor/commands/BehaviorCommands.h"
#include "Source/editor/commands/SceneObjectCommands.h"
#include "Source/editor/material/MaterialDocument.h"
#include "Source/editor/preferences/EditorContentBrowserStore.h"
#include "Source/editor/project/ProjectHub.h"
#include "Source/editor/reflection/ReflectionRegistry.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace editor::ui
{
namespace detail
{
struct EditorUIDrawData final
{
	ImDrawData Data;
	std::vector<std::unique_ptr<ImDrawList>> Lists;
};
} // namespace detail

enum class ContentDialog : uint8
{
	None,
	NewFolder,
	NewMaterial,
	NewMaterialInstance,
	Rename,
	Move,
	Duplicate,
	Trash
};

struct EditorUserInterface::State final
{
	struct ThumbnailTexture final
	{
		asset::AssetThumbnailGPUHandle Handle;
		string SourceHash;
		string Diagnostic;
		std::unique_ptr<ImTextureData> Texture;
		uint64 LastUsedFrame = 0;
	};

	struct ViewportState final
	{
		pipeline::render::RenderViewID View;
		pipeline::render::ViewportSettings Settings{.Overlays = {.Grid = true}};
	};

	struct MaterialColorEditState final
	{
		world::ObjectHandle Object;
		resource::ModelMeshInstanceID MeshInstance = 0;
		resource::MaterialSlotID MaterialSlot = 0;
		util::UUID PreviewID;
		glm::vec4 Value{1.0f};
		bool Dirty = false;
		bool PickerWasOpen = false;
	};

	struct ReflectedPropertyEditState final
	{
		reflection::ReflectionTypeID ComponentType = 0;
		string PropertyName;
		std::vector<world::ObjectHandle> Targets;
		reflection::PropertyValue Value;
		bool Dirty = false;
		bool PickerWasOpen = false;
	};

	core::Window *Window = nullptr;
	std::unique_ptr<EditorUIContext> Context;
	std::unique_ptr<EditorUIRenderer> Renderer;
	std::unique_ptr<EditorUIWindowBridge> WindowBridge;
	std::unique_ptr<preferences::EditorContentBrowserStore> ContentBrowserStore;
	std::unique_ptr<EditorLayoutStore> LayoutStore;
	EditorIconRegistry Icons;
	bool LayoutPrimed = false;
	bool LayoutInitialized = false;
	bool LayoutFailureReported = false;
	bool RecoveryScanned = false;
	bool PreferencesInitialized = false;
	uint64 LayoutResetGeneration = 0;
	string PropertyFilter;
	string AssetFilter;
	std::filesystem::path ContentDirectory;
	std::filesystem::path SelectedContentPath;
	string ContentDialogValue;
	ContentDialog ActiveContentDialog = ContentDialog::None;
	bool ContentDialogRequested = false;
	resource::AssetID ContentParentAssetID;
	resource::AssetType ContentParentAssetType = resource::AssetType::Material;
	bool AssetGridView = false;
	bool FavoritesOnly = false;
	int32 ContentTypeFilter = -1;
	std::unordered_set<resource::AssetID> FavoriteAssetIDs;
	std::vector<resource::AssetID> FavoriteAssetIDsScratch;
	std::unordered_map<asset::AssetThumbnailKey, ThumbnailTexture, asset::AssetThumbnailKeyHash> ThumbnailTextures;
	std::vector<ThumbnailTexture> RetiredThumbnailTextures;
	resource::AssetID InspectedAssetID;
	bool DependencyInspectorRequested = false;
	bool TrashView = false;
	bool TrashLoaded = false;
	std::vector<asset::TrashedContentEntry> TrashEntries;
	std::optional<material::MaterialEditorSession> MaterialSession;
	std::optional<MaterialColorEditState> MaterialColorEdit;
	std::optional<ReflectedPropertyEditState> ReflectedPropertyEdit;
	workspace::EditorWorkspaceID ActiveWorkspace = workspace::EditorWorkspaceID::Home;
	workspace::EditorWorkspaceID RenderedToolbarWorkspace = workspace::EditorWorkspaceID::Count;
	pipeline::render::ViewportSettings DefaultViewportSettings{.Overlays = {.Grid = true}};
	std::vector<ViewportState> Viewports;
	std::vector<core::input::InputEvent> BufferedKeyboardEvents;
	std::vector<core::input::InputEvent> BufferedKeyboardEventsScratch;
	std::mutex BufferedInputMutex;
	pipeline::render::RenderViewID ActiveViewport;
	uint32 RestoredViewportCreations = 0;
	std::unordered_map<util::UUID, bool> ExpandedObjects;
	std::vector<recovery::EditorRecoveryCandidate> RecoveryCandidates;
	std::optional<preferences::EditorPreferences> PreferencesDraft;
	EditorUIFrame FrameScratch;
	std::vector<EditorViewportLayoutState> ViewportLayoutsScratch;
	std::vector<std::shared_ptr<detail::EditorUIDrawData>> DrawDataScratch;
	std::vector<std::pair<asset::AssetThumbnailKey, uint64>> ThumbnailCandidatesScratch;
	std::unordered_map<resource::AssetID, uint64> LiveAssetIDsScratch;
	uint64 LiveAssetIDGeneration = 0;
	std::vector<world::ObjectHandle> ComponentTargetsScratch;
	std::vector<material::PrivateMaterialTarget> PrivateMaterialTargetsScratch;
	std::vector<runtime::behavior::BehaviorDescriptor> BehaviorDescriptorsScratch;
	std::vector<components::BehaviorInstance> BehaviorInstancesScratch;
	std::vector<std::pair<const string *, components::BehaviorPropertyValue *>> BehaviorPropertiesScratch;
	std::vector<util::UUID> MissingComponentObjectsScratch;
	std::vector<uint32> ExplorerOpenDepthsScratch;
	std::vector<const asset::ContentEntry *> ContentEntriesScratch;
	std::vector<core::diagnostics::Diagnostic> DiagnosticsScratch;
	std::vector<const action::EditorActionDescriptor *> ActionDescriptorsScratch;
	string PropertyValueScratch;
	string PropertySearchScratch;
	string PropertyFilterScratch;
	std::vector<string> PropertyListScratch;
	string MaterialNameScratch;
	string HierarchyFilterScratch;
	string ToolbarLabelScratch;
	std::vector<action::EditorActionID> DeferredToolbarActionsScratch;
	string MaterialVirtualPathScratch;
	string ThumbnailDiagnosticScratch;
	string UITextScratch;
	util::UUID ExplorerRenameObject;
	util::UUID ExplorerSelectionAnchor;
	string ExplorerRenameValue;
	bool ExplorerRenameRequested = false;
	string NewProjectName = "New Project";
	std::filesystem::path NewProjectParent;
	string NewProjectParentText;
	bool NewProjectDialogOpen = false;
};

namespace
{
void RetireThumbnailTexture(auto &State, const asset::AssetThumbnailKey &Key)
{
	const auto Found = State.ThumbnailTextures.find(Key);
	if (Found == State.ThumbnailTextures.end())
		return;
	if (Found->second.Texture != nullptr)
	{
		Found->second.Handle.State = asset::AssetThumbnailGPUState::RetirementPending;
		Found->second.Texture->WantDestroyNextFrame = true;
		Found->second.Texture->SetStatus(ImTextureStatus_WantDestroy);
		Found->second.Texture->UnusedFrames = std::max(Found->second.Texture->UnusedFrames, 1);
		State.RetiredThumbnailTextures.push_back(std::move(Found->second));
	}
	State.ThumbnailTextures.erase(Found);
}

void CollectRetiredThumbnailTextures(auto &State)
{
	std::erase_if(State.RetiredThumbnailTextures,
				  [](auto &Retired)
				  {
					  ImTextureData *Texture = Retired.Texture.get();
					  if (Texture->Status != ImTextureStatus_Destroyed)
						  return false;
					  ImGui::UnregisterUserTexture(Texture);
					  Retired.Handle.State = asset::AssetThumbnailGPUState::Retired;
					  return true;
				  });
}

void PruneThumbnailTextures(auto &State, const std::unordered_map<resource::AssetID, uint64> &LiveIDs, const uint64 LiveIDGeneration)
{
	constexpr usize MaximumResidentTextures = 256;
	const auto IsLive = [&LiveIDs, LiveIDGeneration](const resource::AssetID &ID)
	{
		const auto Found = LiveIDs.find(ID);
		return Found != LiveIDs.end() && Found->second == LiveIDGeneration;
	};
	std::vector<std::pair<asset::AssetThumbnailKey, uint64>> &Candidates = State.ThumbnailCandidatesScratch;
	Candidates.clear();
	Candidates.reserve(State.ThumbnailTextures.size());
	for (const auto &[Key, Texture] : State.ThumbnailTextures)
		Candidates.emplace_back(Key, IsLive(Key.ID) ? Texture.LastUsedFrame : 0);
	std::ranges::sort(Candidates, {}, &std::pair<asset::AssetThumbnailKey, uint64>::second);
	usize ResidentCount = State.ThumbnailTextures.size();
	for (const auto &[Key, LastUsedFrame] : Candidates)
	{
		if (IsLive(Key.ID) && ResidentCount <= MaximumResidentTextures)
			break;
		(void)LastUsedFrame;
		RetireThumbnailTexture(State, Key);
		--ResidentCount;
	}
}

void ReleaseThumbnailTextures(auto &State) noexcept
{
	try
	{
		for (auto Iterator = State.ThumbnailTextures.begin(); Iterator != State.ThumbnailTextures.end();)
		{
			const asset::AssetThumbnailKey Key = Iterator->first;
			++Iterator;
			RetireThumbnailTexture(State, Key);
		}
		for (auto &Retired : State.RetiredThumbnailTextures)
		{
			ImTextureData *Texture = Retired.Texture.get();
			if (Texture->Status == ImTextureStatus_OK || Texture->Status == ImTextureStatus_WantUpdates)
			{
				Texture->WantDestroyNextFrame = true;
				Texture->SetStatus(ImTextureStatus_WantDestroy);
				Texture->UnusedFrames = std::max(Texture->UnusedFrames, 1);
			}
			if (Texture->Status == ImTextureStatus_WantDestroy && Texture->TexID != ImTextureID_Invalid)
				ImGui_ImplOpenGL3_UpdateTexture(Texture);
			if (Texture->RefCount > 0)
				ImGui::UnregisterUserTexture(Texture);
			Retired.Handle.State = asset::AssetThumbnailGPUState::Retired;
		}
		State.RetiredThumbnailTextures.clear();
	}
	catch (...)
	{
		std::terminate();
	}
}

[[nodiscard]] ImTextureData *ResolveThumbnailTexture(auto &State, EditorSession &Session, const asset::ContentEntry &Entry,
													 string &Diagnostic)
{
	if (Entry.Kind != asset::ContentEntryKind::Asset || !Entry.AssetType.has_value())
		return nullptr;
	asset::AssetThumbnailService &Service = Session.GetAssetThumbnailService();
	const asset::AssetThumbnailRequest Request{.ID = Entry.ID, .Type = *Entry.AssetType, .SourceHash = Entry.SourceHash};
	const asset::AssetThumbnailKey Key = asset::AssetThumbnailService::MakeKey(Request);
	Service.Request(Request);
	const auto Current = State.ThumbnailTextures.find(Key);
	if (Current != State.ThumbnailTextures.end())
	{
		if (Current->second.Texture != nullptr)
		{
			if (Current->second.Texture->Status == ImTextureStatus_OK)
				Current->second.Handle.State = asset::AssetThumbnailGPUState::Ready;
			else if (Current->second.Texture->Status == ImTextureStatus_WantCreate)
				Current->second.Handle.State = asset::AssetThumbnailGPUState::UploadPending;
		}
		Current->second.LastUsedFrame = static_cast<uint64>(ImGui::GetFrameCount());
		Diagnostic = Current->second.Diagnostic;
		return Current->second.Texture.get();
	}
	const std::shared_ptr<const asset::AssetThumbnailImage> Image = Service.Find(Request);
	if (!Image)
		return nullptr;
	if (!Image->IsValid())
	{
		using ThumbnailTexture = typename std::remove_reference_t<decltype(State.ThumbnailTextures)>::mapped_type;
		State.ThumbnailTextures.emplace(Key, ThumbnailTexture{.Handle = {.Key = Key, .State = asset::AssetThumbnailGPUState::Failed},
															  .SourceHash = Entry.SourceHash,
															  .Diagnostic = Image->Diagnostic,
															  .LastUsedFrame = static_cast<uint64>(ImGui::GetFrameCount())});
		Diagnostic = Image->Diagnostic;
		return nullptr;
	}
	if (Image->Width > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
		Image->Height > static_cast<uint32>(std::numeric_limits<int32>::max()))
	{
		throw std::overflow_error("asset thumbnail dimensions exceed the Dear ImGui texture boundary");
	}
	auto Texture = std::make_unique<ImTextureData>();
	Texture->Create(ImTextureFormat_RGBA32, static_cast<int32>(Image->Width), static_cast<int32>(Image->Height));
	std::memcpy(Texture->GetPixels(), Image->Pixels.data(), Image->Pixels.size());
	Texture->UseColors = true;
	ImGui::RegisterUserTexture(Texture.get());
	ImTextureData *Result = Texture.get();
	using ThumbnailTexture = typename std::remove_reference_t<decltype(State.ThumbnailTextures)>::mapped_type;
	State.ThumbnailTextures.emplace(Key, ThumbnailTexture{.Handle = {.Key = Key,
																	 .Generation = static_cast<uint64>(ImGui::GetFrameCount()),
																	 .State = asset::AssetThumbnailGPUState::UploadPending},
														  .SourceHash = Entry.SourceHash,
														  .Diagnostic = Image->Diagnostic,
														  .Texture = std::move(Texture),
														  .LastUsedFrame = static_cast<uint64>(ImGui::GetFrameCount())});
	return Result;
}

struct KeyMapping final
{
	core::input::Key Source = core::input::Key::Unknown;
	ImGuiKey Destination = ImGuiKey_None;
};

constexpr std::array KeyMappings{KeyMapping{core::input::Key::Tab, ImGuiKey_Tab},
								 KeyMapping{core::input::Key::Left, ImGuiKey_LeftArrow},
								 KeyMapping{core::input::Key::Right, ImGuiKey_RightArrow},
								 KeyMapping{core::input::Key::Up, ImGuiKey_UpArrow},
								 KeyMapping{core::input::Key::Down, ImGuiKey_DownArrow},
								 KeyMapping{core::input::Key::PageUp, ImGuiKey_PageUp},
								 KeyMapping{core::input::Key::PageDown, ImGuiKey_PageDown},
								 KeyMapping{core::input::Key::Home, ImGuiKey_Home},
								 KeyMapping{core::input::Key::End, ImGuiKey_End},
								 KeyMapping{core::input::Key::Insert, ImGuiKey_Insert},
								 KeyMapping{core::input::Key::Delete, ImGuiKey_Delete},
								 KeyMapping{core::input::Key::Backspace, ImGuiKey_Backspace},
								 KeyMapping{core::input::Key::Space, ImGuiKey_Space},
								 KeyMapping{core::input::Key::Enter, ImGuiKey_Enter},
								 KeyMapping{core::input::Key::Escape, ImGuiKey_Escape},
								 KeyMapping{core::input::Key::Apostrophe, ImGuiKey_Apostrophe},
								 KeyMapping{core::input::Key::Comma, ImGuiKey_Comma},
								 KeyMapping{core::input::Key::Minus, ImGuiKey_Minus},
								 KeyMapping{core::input::Key::Period, ImGuiKey_Period},
								 KeyMapping{core::input::Key::Slash, ImGuiKey_Slash},
								 KeyMapping{core::input::Key::Semicolon, ImGuiKey_Semicolon},
								 KeyMapping{core::input::Key::Equal, ImGuiKey_Equal},
								 KeyMapping{core::input::Key::Number0, ImGuiKey_0},
								 KeyMapping{core::input::Key::Number1, ImGuiKey_1},
								 KeyMapping{core::input::Key::Number2, ImGuiKey_2},
								 KeyMapping{core::input::Key::Number3, ImGuiKey_3},
								 KeyMapping{core::input::Key::Number4, ImGuiKey_4},
								 KeyMapping{core::input::Key::Number5, ImGuiKey_5},
								 KeyMapping{core::input::Key::Number6, ImGuiKey_6},
								 KeyMapping{core::input::Key::Number7, ImGuiKey_7},
								 KeyMapping{core::input::Key::Number8, ImGuiKey_8},
								 KeyMapping{core::input::Key::Number9, ImGuiKey_9},
								 KeyMapping{core::input::Key::A, ImGuiKey_A},
								 KeyMapping{core::input::Key::B, ImGuiKey_B},
								 KeyMapping{core::input::Key::C, ImGuiKey_C},
								 KeyMapping{core::input::Key::D, ImGuiKey_D},
								 KeyMapping{core::input::Key::E, ImGuiKey_E},
								 KeyMapping{core::input::Key::F, ImGuiKey_F},
								 KeyMapping{core::input::Key::G, ImGuiKey_G},
								 KeyMapping{core::input::Key::H, ImGuiKey_H},
								 KeyMapping{core::input::Key::I, ImGuiKey_I},
								 KeyMapping{core::input::Key::J, ImGuiKey_J},
								 KeyMapping{core::input::Key::K, ImGuiKey_K},
								 KeyMapping{core::input::Key::L, ImGuiKey_L},
								 KeyMapping{core::input::Key::M, ImGuiKey_M},
								 KeyMapping{core::input::Key::N, ImGuiKey_N},
								 KeyMapping{core::input::Key::O, ImGuiKey_O},
								 KeyMapping{core::input::Key::P, ImGuiKey_P},
								 KeyMapping{core::input::Key::Q, ImGuiKey_Q},
								 KeyMapping{core::input::Key::R, ImGuiKey_R},
								 KeyMapping{core::input::Key::S, ImGuiKey_S},
								 KeyMapping{core::input::Key::T, ImGuiKey_T},
								 KeyMapping{core::input::Key::U, ImGuiKey_U},
								 KeyMapping{core::input::Key::V, ImGuiKey_V},
								 KeyMapping{core::input::Key::W, ImGuiKey_W},
								 KeyMapping{core::input::Key::X, ImGuiKey_X},
								 KeyMapping{core::input::Key::Y, ImGuiKey_Y},
								 KeyMapping{core::input::Key::Z, ImGuiKey_Z},
								 KeyMapping{core::input::Key::F1, ImGuiKey_F1},
								 KeyMapping{core::input::Key::F2, ImGuiKey_F2},
								 KeyMapping{core::input::Key::F3, ImGuiKey_F3},
								 KeyMapping{core::input::Key::F4, ImGuiKey_F4},
								 KeyMapping{core::input::Key::F5, ImGuiKey_F5},
								 KeyMapping{core::input::Key::F6, ImGuiKey_F6},
								 KeyMapping{core::input::Key::F7, ImGuiKey_F7},
								 KeyMapping{core::input::Key::F8, ImGuiKey_F8},
								 KeyMapping{core::input::Key::F9, ImGuiKey_F9},
								 KeyMapping{core::input::Key::F10, ImGuiKey_F10},
								 KeyMapping{core::input::Key::F11, ImGuiKey_F11},
								 KeyMapping{core::input::Key::F12, ImGuiKey_F12},
								 KeyMapping{core::input::Key::LeftShift, ImGuiKey_LeftShift},
								 KeyMapping{core::input::Key::LeftControl, ImGuiKey_LeftCtrl},
								 KeyMapping{core::input::Key::LeftAlt, ImGuiKey_LeftAlt},
								 KeyMapping{core::input::Key::LeftSuper, ImGuiKey_LeftSuper},
								 KeyMapping{core::input::Key::RightShift, ImGuiKey_RightShift},
								 KeyMapping{core::input::Key::RightControl, ImGuiKey_RightCtrl},
								 KeyMapping{core::input::Key::RightAlt, ImGuiKey_RightAlt},
								 KeyMapping{core::input::Key::RightSuper, ImGuiKey_RightSuper},
								 KeyMapping{core::input::Key::Menu, ImGuiKey_Menu}};

[[nodiscard]] bool InputText(const string_view Label, string &Value)
{
	return ImGui::InputText(Label.data(), &Value);
}

void FeedInput(auto &State, const core::ApplicationFrame &Frame, const core::Window &Window, const EditorUIWindowBridge &WindowBridge)
{
	const core::input::InputSnapshot *PointerInput = &Frame.Input;
	const core::input::InputSnapshot *KeyboardInput = &Frame.Input;
	const core::Window *PointerWindow = &Window;
	ImGuiID HoveredViewport = 0;
	for (ImGuiViewport *Viewport : ImGui::GetPlatformIO().Viewports)
	{
		const core::Window *ManagedWindow = WindowBridge.GetManagedWindow(*Viewport);
		if (ManagedWindow == nullptr)
			continue;
		const core::input::InputSnapshot &Candidate = Frame.Services.GetInputSystem().GetSnapshot(ManagedWindow->GetID());
		if (ManagedWindow->IsFocused())
			KeyboardInput = &Candidate;
		if (Candidate.IsCursorInside())
		{
			PointerInput = &Candidate;
			PointerWindow = ManagedWindow;
			HoveredViewport = Viewport->ID;
		}
	}
	ImGuiIO &IO = ImGui::GetIO();
	const core::WindowExtent ClientExtent = Window.GetExtent();
	const core::WindowExtent FramebufferExtent = Frame.FramebufferExtent;
	IO.DisplaySize = ImVec2(static_cast<float32>(ClientExtent.Width), static_cast<float32>(ClientExtent.Height));
	IO.DisplayFramebufferScale = ClientExtent.IsValid()
									 ? ImVec2(static_cast<float32>(FramebufferExtent.Width) / static_cast<float32>(ClientExtent.Width),
											  static_cast<float32>(FramebufferExtent.Height) / static_cast<float32>(ClientExtent.Height))
									 : ImVec2(1.0f, 1.0f);
	IO.DeltaTime = static_cast<float32>(std::max(Frame.Timing.DeltaSeconds, 1.0 / 1'000.0));

	const core::WindowPosition PointerWindowPosition = PointerWindow->GetPosition();
	IO.AddMousePosEvent(static_cast<float32>(PointerInput->GetMouseX()) + static_cast<float32>(PointerWindowPosition.X),
						static_cast<float32>(PointerInput->GetMouseY()) + static_cast<float32>(PointerWindowPosition.Y));
	IO.AddMouseViewportEvent(HoveredViewport);
	const uint8 BridgedMouseButtonCount =
		std::min(static_cast<uint8>(core::input::MouseButton::Count), static_cast<uint8>(ImGuiMouseButton_COUNT));
	for (uint8 Index = 0; Index < BridgedMouseButtonCount; ++Index)
	{
		const auto Button = static_cast<core::input::MouseButton>(Index);
		IO.AddMouseButtonEvent(Index, PointerInput->IsMouseButtonDown(Button));
	}
	IO.AddMouseWheelEvent(static_cast<float32>(PointerInput->GetScrollX()), static_cast<float32>(PointerInput->GetScrollY()));
	for (const KeyMapping &Mapping : KeyMappings)
		IO.AddKeyEvent(Mapping.Destination, KeyboardInput->IsKeyDown(Mapping.Source));
	const core::input::Modifier Modifiers = KeyboardInput->GetModifiers();
	IO.AddKeyEvent(ImGuiMod_Ctrl, core::input::Contains(Modifiers, core::input::Modifier::Control));
	IO.AddKeyEvent(ImGuiMod_Shift, core::input::Contains(Modifiers, core::input::Modifier::Shift));
	IO.AddKeyEvent(ImGuiMod_Alt, core::input::Contains(Modifiers, core::input::Modifier::Alt));
	IO.AddKeyEvent(ImGuiMod_Super, core::input::Contains(Modifiers, core::input::Modifier::Super));
	{
		std::scoped_lock InputLock(State.BufferedInputMutex);
		State.BufferedKeyboardEventsScratch.clear();
		State.BufferedKeyboardEventsScratch.swap(State.BufferedKeyboardEvents);
	}
	for (const core::input::InputEvent &Event : State.BufferedKeyboardEventsScratch)
	{
		if (Event.Window != KeyboardInput->GetWindowID())
			continue;
		if (Event.Type == core::input::InputEventType::Key)
		{
			const auto Mapping = std::ranges::find(KeyMappings, Event.Key, &KeyMapping::Source);
			if (Mapping != KeyMappings.end())
				IO.AddKeyEvent(Mapping->Destination, Event.State != core::input::InputState::Released);
		}
		else if (Event.Type == core::input::InputEventType::Text && Event.Codepoint <= 0x10FFFFU &&
				 (Event.Codepoint < 0xD800U || Event.Codepoint > 0xDFFFU))
		{
			IO.AddInputCharacter(Event.Codepoint);
		}
	}
	State.BufferedKeyboardEventsScratch.clear();

	const core::input::ControllerSnapshot &Controller = KeyboardInput->GetController();
	const bool ControllerConnected = Controller.Connected;
	if (ControllerConnected)
		IO.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	else
		IO.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
	const auto AddControllerButton = [&IO, &Controller, ControllerConnected](const ImGuiKey Key, const core::input::ControllerButton Button)
	{ IO.AddKeyEvent(Key, ControllerConnected && Controller.Buttons[static_cast<usize>(Button)].Down); };
	AddControllerButton(ImGuiKey_GamepadDpadUp, core::input::ControllerButton::DPadUp);
	AddControllerButton(ImGuiKey_GamepadDpadDown, core::input::ControllerButton::DPadDown);
	AddControllerButton(ImGuiKey_GamepadDpadLeft, core::input::ControllerButton::DPadLeft);
	AddControllerButton(ImGuiKey_GamepadDpadRight, core::input::ControllerButton::DPadRight);
	AddControllerButton(ImGuiKey_GamepadStart, core::input::ControllerButton::Start);
	AddControllerButton(ImGuiKey_GamepadBack, core::input::ControllerButton::Back);
	AddControllerButton(ImGuiKey_GamepadL3, core::input::ControllerButton::LeftThumb);
	AddControllerButton(ImGuiKey_GamepadR3, core::input::ControllerButton::RightThumb);
	AddControllerButton(ImGuiKey_GamepadL1, core::input::ControllerButton::LeftShoulder);
	AddControllerButton(ImGuiKey_GamepadR1, core::input::ControllerButton::RightShoulder);
	AddControllerButton(ImGuiKey_GamepadFaceDown, core::input::ControllerButton::FaceDown);
	AddControllerButton(ImGuiKey_GamepadFaceRight, core::input::ControllerButton::FaceRight);
	AddControllerButton(ImGuiKey_GamepadFaceLeft, core::input::ControllerButton::FaceLeft);
	AddControllerButton(ImGuiKey_GamepadFaceUp, core::input::ControllerButton::FaceUp);
	const auto AddControllerAxis = [&IO](const ImGuiKey NegativeKey, const ImGuiKey PositiveKey, const float32 Value)
	{
		constexpr float32 DeadZone = 0.15f;
		const float32 Negative = std::max(0.0f, -Value);
		const float32 Positive = std::max(0.0f, Value);
		IO.AddKeyAnalogEvent(NegativeKey, Negative > DeadZone, Negative);
		IO.AddKeyAnalogEvent(PositiveKey, Positive > DeadZone, Positive);
	};
	const auto &Axes = Controller.Axes;
	const auto AxisValue = [ControllerConnected, &Axes](const core::input::ControllerAxis Axis)
	{ return ControllerConnected ? Axes[static_cast<usize>(Axis)] : 0.0f; };
	AddControllerAxis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, AxisValue(core::input::ControllerAxis::LeftX));
	AddControllerAxis(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, -AxisValue(core::input::ControllerAxis::LeftY));
	AddControllerAxis(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, AxisValue(core::input::ControllerAxis::RightX));
	AddControllerAxis(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, -AxisValue(core::input::ControllerAxis::RightY));
	const float32 LeftTrigger = AxisValue(core::input::ControllerAxis::LeftTrigger);
	const float32 RightTrigger = AxisValue(core::input::ControllerAxis::RightTrigger);
	IO.AddKeyAnalogEvent(ImGuiKey_GamepadL2, LeftTrigger > 0.15f, LeftTrigger);
	IO.AddKeyAnalogEvent(ImGuiKey_GamepadR2, RightTrigger > 0.15f, RightTrigger);
}

void CloneDrawDataInto(const ImDrawData &Source, detail::EditorUIDrawData &Result)
{
	Result.Data.Valid = Source.Valid;
	Result.Data.DisplayPos = Source.DisplayPos;
	Result.Data.DisplaySize = Source.DisplaySize;
	Result.Data.FramebufferScale = Source.FramebufferScale;
	Result.Data.OwnerViewport = nullptr;
	Result.Data.Textures = nullptr;
	Result.Data.CmdLists.Size = 0;
	Result.Data.CmdLists.resize(Source.CmdLists.Size);
	Result.Data.TotalIdxCount = 0;
	Result.Data.TotalVtxCount = 0;
	for (int32 Index = 0; Index < Source.CmdLists.Size; ++Index)
	{
		if (static_cast<usize>(Index) >= Result.Lists.size())
			Result.Lists.emplace_back(std::make_unique<ImDrawList>(nullptr));
		const ImDrawList *SourceList = Source.CmdLists[Index];
		if (SourceList == nullptr)
			throw std::logic_error("ImGui supplied a null draw list");
		ImDrawList *ClonedList = Result.Lists[Index].get();
		ClonedList->CmdBuffer.Size = 0;
		ClonedList->CmdBuffer.resize(SourceList->CmdBuffer.Size);
		if (SourceList->CmdBuffer.Size != 0)
			std::memcpy(ClonedList->CmdBuffer.Data, SourceList->CmdBuffer.Data,
						static_cast<usize>(SourceList->CmdBuffer.Size) * sizeof(ImDrawCmd));
		ClonedList->IdxBuffer.Size = 0;
		ClonedList->IdxBuffer.resize(SourceList->IdxBuffer.Size);
		if (SourceList->IdxBuffer.Size != 0)
			std::memcpy(ClonedList->IdxBuffer.Data, SourceList->IdxBuffer.Data,
						static_cast<usize>(SourceList->IdxBuffer.Size) * sizeof(ImDrawIdx));
		ClonedList->VtxBuffer.Size = 0;
		ClonedList->VtxBuffer.resize(SourceList->VtxBuffer.Size);
		if (SourceList->VtxBuffer.Size != 0)
			std::memcpy(ClonedList->VtxBuffer.Data, SourceList->VtxBuffer.Data,
						static_cast<usize>(SourceList->VtxBuffer.Size) * sizeof(ImDrawVert));
		ClonedList->Flags = SourceList->Flags;
		Result.Data.CmdLists[Index] = ClonedList;
		Result.Data.TotalIdxCount += ClonedList->IdxBuffer.Size;
		Result.Data.TotalVtxCount += ClonedList->VtxBuffer.Size;
	}
	Result.Data.CmdListsCount = Result.Data.CmdLists.Size;
}

void FormatPropertyValue(const reflection::PropertyValue &Value, string &Result)
{
	Result.clear();
	std::visit(
		[&Result]<typename ValueType>(const ValueType &Typed)
		{
			if constexpr (std::same_as<ValueType, bool>)
				Result = Typed ? "True" : "False";
			else if constexpr (std::integral<ValueType> || std::floating_point<ValueType>)
				std::format_to(std::back_inserter(Result), "{}", Typed);
			else if constexpr (std::same_as<ValueType, string>)
				Result = Typed;
			else if constexpr (std::same_as<ValueType, std::vector<string>>)
			{
				for (const string &Entry : Typed)
				{
					if (!Result.empty())
						Result += ", ";
					Result += Entry;
				}
			}
			else if constexpr (std::same_as<ValueType, glm::vec2>)
				std::format_to(std::back_inserter(Result), "{:.3f}, {:.3f}", Typed.x, Typed.y);
			else if constexpr (std::same_as<ValueType, glm::vec3>)
				std::format_to(std::back_inserter(Result), "{:.3f}, {:.3f}, {:.3f}", Typed.x, Typed.y, Typed.z);
			else if constexpr (std::same_as<ValueType, glm::vec4> || std::same_as<ValueType, glm::quat>)
				std::format_to(std::back_inserter(Result), "{:.3f}, {:.3f}, {:.3f}, {:.3f}", Typed.x, Typed.y, Typed.z, Typed.w);
			else if constexpr (std::same_as<ValueType, util::UUID>)
				Result = Typed.ToString();
			else if constexpr (std::same_as<ValueType, world::ObjectHandle>)
			{
				if (Typed.IsValid())
					std::format_to(std::back_inserter(Result), "Object {}:{}", Typed.Slot, Typed.Generation);
				else
					Result = "None";
			}
			else
				Result = Typed.ID.empty() ? "None" : Typed.ID;
		},
		Value);
}

[[nodiscard]] bool RenderPropertyEditor(const reflection::PropertyDescriptor &Property, reflection::PropertyValue &Value,
										string &TextScratch, std::vector<string> &ParsedScratch)
{
	const bool ReadOnly = reflection::HasFlag(Property.Flags, reflection::PropertyFlags::ReadOnly) || !Property.Write;
	ImGui::BeginDisabled(ReadOnly);
	bool Changed = false;
	if (auto *BooleanValue = std::get_if<bool>(&Value))
		Changed = ImGui::Checkbox("##Value", BooleanValue);
	else if (auto *Signed32Value = std::get_if<int32>(&Value))
		Changed = ImGui::InputScalar("##Value", ImGuiDataType_S32, Signed32Value);
	else if (auto *Unsigned32Value = std::get_if<uint32>(&Value))
	{
		if (!Property.EnumOptions.empty() && reflection::HasFlag(Property.Flags, reflection::PropertyFlags::Bitmask))
		{
			string &Preview = TextScratch;
			Preview.clear();
			for (const reflection::EnumPropertyOption &Option : Property.EnumOptions)
			{
				if ((*Unsigned32Value & static_cast<uint32>(Option.Value)) == 0)
					continue;
				if (!Preview.empty())
					Preview += ", ";
				Preview += Option.DisplayName;
			}
			if (Preview.empty())
				Preview = "None";
			if (ImGui::BeginCombo("##Value", Preview.c_str()))
			{
				for (const reflection::EnumPropertyOption &Option : Property.EnumOptions)
				{
					const uint32 Bit = static_cast<uint32>(Option.Value);
					const bool Selected = (*Unsigned32Value & Bit) != 0;
					if (ImGui::Selectable(Option.DisplayName.c_str(), Selected, ImGuiSelectableFlags_DontClosePopups))
					{
						*Unsigned32Value = Selected ? (*Unsigned32Value & ~Bit) : (*Unsigned32Value | Bit);
						Changed = true;
					}
				}
				ImGui::EndCombo();
			}
		}
		else if (!Property.EnumOptions.empty())
		{
			const auto Selected =
				std::ranges::find(Property.EnumOptions, static_cast<uint64>(*Unsigned32Value), &reflection::EnumPropertyOption::Value);
			const string_view Preview = Selected == Property.EnumOptions.end() ? "Unknown" : string_view(Selected->DisplayName);
			if (ImGui::BeginCombo("##Value", Preview.data()))
			{
				for (const reflection::EnumPropertyOption &Option : Property.EnumOptions)
				{
					const bool IsSelected = Option.Value == *Unsigned32Value;
					if (ImGui::Selectable(Option.DisplayName.c_str(), IsSelected))
					{
						*Unsigned32Value = static_cast<uint32>(Option.Value);
						Changed = true;
					}
					if (IsSelected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}
		else
			Changed = ImGui::InputScalar("##Value", ImGuiDataType_U32, Unsigned32Value);
	}
	else if (auto *Signed64Value = std::get_if<int64>(&Value))
		Changed = ImGui::InputScalar("##Value", ImGuiDataType_S64, Signed64Value);
	else if (auto *Unsigned64Value = std::get_if<uint64>(&Value))
		Changed = ImGui::InputScalar("##Value", ImGuiDataType_U64, Unsigned64Value);
	else if (auto *Scalar32Value = std::get_if<float32>(&Value))
	{
		const float32 Speed = static_cast<float32>(Property.Numeric.Step.value_or(0.05));
		const float32 Minimum = static_cast<float32>(Property.Numeric.Minimum.value_or(0.0));
		const float32 Maximum = static_cast<float32>(Property.Numeric.Maximum.value_or(0.0));
		Changed = ImGui::DragFloat("##Value", Scalar32Value, Speed, Minimum, Maximum);
	}
	else if (auto *Scalar64Value = std::get_if<float64>(&Value))
		Changed = ImGui::InputDouble("##Value", Scalar64Value, Property.Numeric.Step.value_or(0.0));
	else if (auto *StringValue = std::get_if<string>(&Value))
		Changed = InputText("##Value", *StringValue);
	else if (auto *StringListValue = std::get_if<std::vector<string>>(&Value))
	{
		FormatPropertyValue(Value, TextScratch);
		if (InputText("##Value", TextScratch))
		{
			ParsedScratch.clear();
			usize Start = 0;
			while (Start <= TextScratch.size())
			{
				const usize Separator = TextScratch.find(',', Start);
				string Entry = TextScratch.substr(Start, Separator == string::npos ? string::npos : Separator - Start);
				const auto NotWhitespace = [](const char Character) { return std::isspace(static_cast<unsigned char>(Character)) == 0; };
				const auto First = std::ranges::find_if(Entry, NotWhitespace);
				const auto Last = std::ranges::find_if(Entry | std::views::reverse, NotWhitespace).base();
				if (First < Last)
				{
					Entry = string(First, Last);
					if (std::ranges::find(ParsedScratch, Entry) == ParsedScratch.end())
						ParsedScratch.push_back(std::move(Entry));
				}
				if (Separator == string::npos)
					break;
				Start = Separator + 1U;
			}
			*StringListValue = ParsedScratch;
			Changed = true;
		}
	}
	else if (auto *Vector2Value = std::get_if<glm::vec2>(&Value))
		Changed = ImGui::DragFloat2("##Value", &Vector2Value->x, static_cast<float32>(Property.Numeric.Step.value_or(0.05)));
	else if (auto *Vector3Value = std::get_if<glm::vec3>(&Value))
	{
		Changed = Property.Kind == reflection::PropertyKind::Color ? ImGui::ColorEdit3("##Value", &Vector3Value->x)
																   : ImGui::DragFloat3("##Value", &Vector3Value->x, 0.05f);
	}
	else if (auto *Vector4Value = std::get_if<glm::vec4>(&Value))
		Changed = Property.Kind == reflection::PropertyKind::Color ? ImGui::ColorEdit4("##Value", &Vector4Value->x)
																   : ImGui::DragFloat4("##Value", &Vector4Value->x, 0.05f);
	else if (auto *QuaternionValue = std::get_if<glm::quat>(&Value))
		Changed = ImGui::DragFloat4("##Value", &QuaternionValue->x, 0.01f);
	else
	{
		FormatPropertyValue(Value, TextScratch);
		ImGui::TextUnformatted(TextScratch.c_str());
	}
	ImGui::EndDisabled();
	return Changed && !ReadOnly;
}

[[nodiscard]] bool PropertyValuesEqual(const reflection::PropertyValue &Left, const reflection::PropertyValue &Right)
{
	if (Left.index() != Right.index())
		return false;
	return std::visit(
		[&Right](const auto &LeftValue)
		{
			using ValueType = std::remove_cvref_t<decltype(LeftValue)>;
			const ValueType &RightValue = std::get<ValueType>(Right);
			if constexpr (std::same_as<ValueType, glm::vec2> || std::same_as<ValueType, glm::vec3> || std::same_as<ValueType, glm::vec4>)
				return glm::all(glm::equal(LeftValue, RightValue));
			else if constexpr (std::same_as<ValueType, glm::quat>)
				return LeftValue.x == RightValue.x && LeftValue.y == RightValue.y && LeftValue.z == RightValue.z &&
					   LeftValue.w == RightValue.w;
			else
				return LeftValue == RightValue;
		},
		Left);
}

void OpenColorEditPickerPopup(const char *Label)
{
	ImGui::PushID(Label);
	ImGui::OpenPopup("picker");
	ImGui::PopID();
}

[[nodiscard]] bool IsColorEditPickerPopupOpen(const char *Label)
{
	ImGui::PushID(Label);
	const bool Open = ImGui::IsPopupOpen("picker");
	ImGui::PopID();
	return Open;
}

[[nodiscard]] bool RenderPanelMinimizeControl(EditorSession &Session, workspace::EditorPanelState &Panel)
{
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()));
	ImGui::PushID(static_cast<int32>(Panel.ID));
	const bool Minimize = ImGui::SmallButton("-");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Minimize %s", Panel.Name.c_str());
	ImGui::PopID();
	ImGui::Separator();
	if (Minimize)
		Session.GetWorkspace().SetMinimized(Panel.ID, true);
	return Minimize;
}

template <IsCObjectComponent ComponentType>
void RenderComponent(auto &State, EditorSession &Session, action::EditorActionContext &Context, const world::ObjectHandle Object,
					 const string_view PropertyFilter)
{
	world::Scene &Scene = Session.GetDocument().GetScene();
	const world::ComponentHandle<ComponentType> Handle = Scene.GetComponent<ComponentType>(Object);
	if (!Handle.IsValid())
		return;
	std::vector<world::ObjectHandle> &Targets = State.ComponentTargetsScratch;
	Targets.clear();
	for (const util::UUID &ID : Session.GetDocument().GetSelection().GetOrdered())
	{
		const world::ObjectHandle Target = Scene.FindObject(ID);
		if (Target.IsValid() && Scene.GetComponent<ComponentType>(Target).IsValid())
			Targets.push_back(Target);
	}
	if (Targets.empty())
		return;
	const std::optional<reflection::TypeDescriptor> Descriptor =
		Session.GetReflection().Find("components." + string(ComponentType::ComponentName));
	if (!Descriptor.has_value())
		return;

	constexpr bool Removable = !std::same_as<ComponentType, components::CObjectIdentityComponent> &&
							   !std::same_as<ComponentType, components::CObjectTransformComponent> &&
							   !std::same_as<ComponentType, components::CObjectHierarchyComponent>;
	bool KeepAttached = true;
	const bool Expanded = [&]()
	{
		if constexpr (Removable)
			return ImGui::CollapsingHeader(Descriptor->DisplayName.c_str(), &KeepAttached, ImGuiTreeNodeFlags_DefaultOpen);
		else
			return ImGui::CollapsingHeader(Descriptor->DisplayName.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
	}();
	if constexpr (Removable)
	{
		if (!KeepAttached)
		{
			try
			{
				commands::CommandHistory &History = Session.GetDocument().GetHistory();
				History.BeginTransaction("Remove " + Descriptor->DisplayName);
				for (const world::ObjectHandle Target : Targets)
				{
					const auto Identity = Scene.GetComponent<components::CObjectIdentityComponent>(Target);
					const util::UUID ID = [&Scene, Identity]()
					{
						auto Access = Scene.Read();
						return Access.Resolve(Identity).GetPersistentID();
					}();
					History.Execute(std::make_unique<commands::RemoveComponentCommand>(Session.GetDocument(), ID, ComponentType::TypeID));
				}
				History.CommitTransaction();
			}
			catch (const std::exception &Exception)
			{
				if (Session.GetDocument().GetHistory().HasOpenTransaction())
					Session.GetDocument().GetHistory().CancelTransaction();
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
			}
			return;
		}
	}
	if (!Expanded)
		return;
	string_view CurrentCategory;
	for (const reflection::PropertyDescriptor &Property : Descriptor->Properties)
	{
		if (reflection::HasFlag(Property.Flags, reflection::PropertyFlags::Hidden))
			continue;
		if (!PropertyFilter.empty())
		{
			string &Search = State.PropertySearchScratch;
			Search.assign(Property.DisplayName);
			Search.push_back(' ');
			Search.append(Property.Category);
			std::ranges::transform(Search, Search.begin(), [](const char Character)
								   { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
			string &Filter = State.PropertyFilterScratch;
			Filter.assign(PropertyFilter);
			std::ranges::transform(Filter, Filter.begin(), [](const char Character)
								   { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
			if (Search.find(Filter) == string::npos)
				continue;
		}
		if (CurrentCategory != Property.Category)
		{
			CurrentCategory = Property.Category;
			ImGui::SeparatorText(CurrentCategory.data());
		}

		reflection::PropertyValue Value;
		bool Mixed = false;
		{
			auto Access = Scene.Read();
			const ComponentType &Component = Access.Resolve(Handle);
			Value = Property.Read(&Component);
			for (const world::ObjectHandle Target : Targets)
			{
				const ComponentType &TargetComponent = Access.Resolve(Access.GetComponent<ComponentType>(Target));
				if (!PropertyValuesEqual(Value, Property.Read(&TargetComponent)))
				{
					Mixed = true;
					break;
				}
			}
		}
		const bool BufferedEditMatches =
			State.ReflectedPropertyEdit.has_value() && State.ReflectedPropertyEdit->ComponentType == ComponentType::TypeID &&
			State.ReflectedPropertyEdit->PropertyName == Property.Name && State.ReflectedPropertyEdit->Targets == Targets;
		if (BufferedEditMatches)
			Value = State.ReflectedPropertyEdit->Value;
		ImGui::PushID(Property.Name.c_str());
		ImGui::Columns(2, "PropertyColumns", false);
		ImGui::SetColumnWidth(0, std::max(115.0f, ImGui::GetContentRegionAvail().x * 0.42f));
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(Property.DisplayName.c_str());
		ImGui::NextColumn();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, Mixed);
		bool Changed = RenderPropertyEditor(Property, Value, State.PropertyValueScratch, State.PropertyListScratch);
		const bool EditFinished = ImGui::IsItemDeactivatedAfterEdit();
		const bool ItemActive = ImGui::IsItemActive();
		ImGui::PopItemFlag();
		const bool ColorPickerOpen = Property.Kind == reflection::PropertyKind::Color && IsColorEditPickerPopupOpen("##Value");
		bool Reset = false;
		if (ImGui::BeginPopupContextItem("PropertyContext"))
		{
			Reset = ImGui::MenuItem("Reset to Default", nullptr, false,
									Property.DefaultValue.has_value() && Property.Write &&
										!reflection::HasFlag(Property.Flags, reflection::PropertyFlags::ReadOnly));
			ImGui::EndPopup();
		}
		if (Property.Kind == reflection::PropertyKind::AssetReference && Property.Write && ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload *Payload = ImGui::AcceptDragDropPayload("ASSET_CONTENT_PATH"))
			{
				const string Path(static_cast<const char *>(Payload->Data),
								  (static_cast<const char *>(Payload->Data) + Payload->DataSize) - 1);
				const asset::AssetRegistrySnapshot &Snapshot = Session.GetAssetRegistry().GetSnapshot();
				const auto Entry = std::ranges::find(Snapshot.Entries, std::filesystem::path(Path), &asset::ContentEntry::RelativePath);
				auto *Reference = std::get_if<reflection::AssetReference>(&Value);
				if (Entry != Snapshot.Entries.end() && Reference != nullptr && Entry->AssetType == Reference->Type)
				{
					Reference->ID = Entry->ID;
					Changed = true;
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::Columns(1);
		ImGui::PopID();
		if (Reset)
		{
			Value = *Property.DefaultValue;
			Changed = true;
		}
		if (Changed)
		{
			if (!BufferedEditMatches)
			{
				State.ReflectedPropertyEdit.emplace();
				State.ReflectedPropertyEdit->ComponentType = ComponentType::TypeID;
				State.ReflectedPropertyEdit->PropertyName = Property.Name;
				State.ReflectedPropertyEdit->Targets = Targets;
				State.ReflectedPropertyEdit->Value = Value;
				State.ReflectedPropertyEdit->Dirty = true;
				State.ReflectedPropertyEdit->PickerWasOpen = ColorPickerOpen;
			}
			else
			{
				State.ReflectedPropertyEdit->Value = Value;
				State.ReflectedPropertyEdit->Dirty = true;
			}
		}
		const bool CurrentEditMatches =
			State.ReflectedPropertyEdit.has_value() && State.ReflectedPropertyEdit->ComponentType == ComponentType::TypeID &&
			State.ReflectedPropertyEdit->PropertyName == Property.Name && State.ReflectedPropertyEdit->Targets == Targets;
		const bool ColorPickerClosed = CurrentEditMatches && State.ReflectedPropertyEdit->PickerWasOpen && !ColorPickerOpen;
		const bool InlineColorEditFinished = CurrentEditMatches && !State.ReflectedPropertyEdit->PickerWasOpen && !ColorPickerOpen &&
											 (EditFinished || (Changed && !ItemActive));
		const bool CommitEdit = CurrentEditMatches && State.ReflectedPropertyEdit->Dirty &&
								(Reset || (Property.Kind == reflection::PropertyKind::Color ? ColorPickerClosed || InlineColorEditFinished
																							: EditFinished || (Changed && !ItemActive)));
		if (CurrentEditMatches && ColorPickerOpen)
			State.ReflectedPropertyEdit->PickerWasOpen = true;
		if (CommitEdit)
		{
			reflection::PropertyValue CommittedValue = std::move(State.ReflectedPropertyEdit->Value);
			State.ReflectedPropertyEdit.reset();
			try
			{
				Session.GetDocument().Execute(commands::PropertyEditCommand::Create(
					Scene, Targets, ComponentType::TypeID, Property, std::move(CommittedValue), &Session.GetProject().GetAssetManager()));
			}
			catch (const std::exception &Exception)
			{
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
			}
		}
	}
}

void RenderMeshMaterials(auto &State, EditorSession &Session, action::EditorActionContext &Context, const world::ObjectHandle Object)
{
	world::Scene &Scene = Session.GetDocument().GetScene();
	const auto PrimaryHandle = Scene.GetComponent<components::CObjectMeshComponent>(Object);
	if (!PrimaryHandle.IsValid() || !ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	resource::AssetHandle<resource::ModelAsset> Model;
	std::vector<world::ObjectHandle> &Targets = State.ComponentTargetsScratch;
	Targets.clear();
	{
		auto Access = Scene.Read();
		Model = Access.Resolve(PrimaryHandle).GetModel();
		const resource::AssetID ModelID = Model.GetID();
		for (const util::UUID &ID : Session.GetDocument().GetSelection().GetOrdered())
		{
			const world::ObjectHandle Target = Scene.FindObject(ID);
			const auto Mesh = Target.IsValid() ? Access.GetComponent<components::CObjectMeshComponent>(Target)
											   : world::ComponentHandle<components::CObjectMeshComponent>{};
			if (Mesh.IsValid() && Access.Resolve(Mesh).GetModel().GetID() == ModelID)
				Targets.push_back(Target);
		}
	}

	const auto ApplyMaterial = [&](const resource::ModelMeshInstanceID MeshInstance, const resource::MaterialSlotID MaterialSlot,
								   resource::AssetHandle<resource::MaterialInterfaceAsset> Material)
	{
		try
		{
			Session.GetDocument().Execute(
				std::make_unique<commands::MeshMaterialOverrideCommand>(Scene, Targets, MeshInstance, MaterialSlot, std::move(Material)));
		}
		catch (const std::exception &Exception)
		{
			Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
		}
	};

	bool FocusBaseColor = Session.ConsumeMaterialColorFocusRequest();
	const auto PinnedModel = Model.TryPin();
	if (!PinnedModel)
	{
		ImGui::TextDisabled("The model asset is temporarily unavailable.");
		return;
	}
	for (const resource::ModelMeshInstance &Instance : PinnedModel->GetMeshInstances())
	{
		const auto Mesh = Instance.Mesh.TryPin();
		if (!Mesh)
			continue;
		for (const resource::MeshMaterialSlot &Slot : Mesh->GetMaterialSlots())
		{
			ImGui::PushID(&Instance);
			ImGui::PushID(&Slot);
			std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>> Override;
			{
				auto Access = Scene.Read();
				const auto &Component = Access.Resolve(PrimaryHandle);
				const auto Found = std::ranges::find_if(Component.GetMaterialOverrides(), [&](const components::MeshMaterialOverride &Value)
														{ return Value.MeshInstance == Instance.ID && Value.MaterialSlot == Slot.ID; });
				if (Found != Component.GetMaterialOverrides().end())
					Override = Found->Material;
			}
			const resource::AssetHandle<resource::MaterialInterfaceAsset> Effective = Override.value_or(Slot.DefaultMaterial);
			string &MaterialName = State.MaterialNameScratch;
			MaterialName = "None";
			if (Effective)
			{
				if (const auto Pinned = Effective.TryPin())
					MaterialName.assign(Pinned->GetName());
				else
					MaterialName.assign(Effective.GetID());
			}
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(Slot.Name.c_str());
			ImGui::SameLine(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.38f));
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::Button(MaterialName.c_str(), ImVec2(-1.0f, 0.0f));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Drop a Material or Material Instance. Double-click to edit. Right-click to clear the override.");
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && Effective)
			{
				const asset::ContentEntry *Entry = Session.GetAssetRegistry().GetSnapshot().Find(Effective.GetID());
				if (Entry != nullptr &&
					(Entry->AssetType == resource::AssetType::Material || Entry->AssetType == resource::AssetType::MaterialInstance))
				{
					try
					{
						State.MaterialSession = material::MaterialEditorSession::Open(
							Session.GetProject().ResolveContentPath(Entry->RelativePath), Session.GetProject().GetAssetManager());
						Session.GetWorkspace().SetOpen(workspace::EditorPanelID::MaterialEditor, true);
					}
					catch (const std::exception &Exception)
					{
						Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
					}
				}
			}
			if (ImGui::BeginPopupContextItem("MaterialSlotContext"))
			{
				if (ImGui::MenuItem("Clear Override", nullptr, false, Override.has_value()))
					ApplyMaterial(Instance.ID, Slot.ID, {});
				ImGui::EndPopup();
			}
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload *Payload = ImGui::AcceptDragDropPayload("ASSET_CONTENT_PATH"))
				{
					const string Path(static_cast<const char *>(Payload->Data),
									  (static_cast<const char *>(Payload->Data) + Payload->DataSize) - 1);
					const auto Entry = std::ranges::find(Session.GetAssetRegistry().GetSnapshot().Entries, std::filesystem::path(Path),
														 &asset::ContentEntry::RelativePath);
					try
					{
						if (Entry != Session.GetAssetRegistry().GetSnapshot().Entries.end() &&
							Entry->AssetType == resource::AssetType::Material)
							ApplyMaterial(Instance.ID, Slot.ID,
										  Session.GetProject().GetAssetManager().GetAssetByID<resource::MaterialAsset>(Entry->ID));
						else if (Entry != Session.GetAssetRegistry().GetSnapshot().Entries.end() &&
								 Entry->AssetType == resource::AssetType::MaterialInstance)
							ApplyMaterial(Instance.ID, Slot.ID,
										  Session.GetProject().GetAssetManager().GetAssetByID<resource::MaterialInstanceAsset>(Entry->ID));
						else
							throw std::invalid_argument("Material slots accept only Material or Material Instance assets");
					}
					catch (const std::exception &Exception)
					{
						Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
					}
				}
				ImGui::EndDragDropTarget();
			}

			const bool MatchingColorEdit = State.MaterialColorEdit.has_value() && State.MaterialColorEdit->Object == Object &&
										   State.MaterialColorEdit->MeshInstance == Instance.ID &&
										   State.MaterialColorEdit->MaterialSlot == Slot.ID;
			const auto EffectiveMaterial = Effective.TryPin();
			glm::vec4 BaseColor = MatchingColorEdit ? State.MaterialColorEdit->Value
													: (EffectiveMaterial ? EffectiveMaterial->GetFactors().BaseColor : glm::vec4(1.0f));
			bool MixedBaseColor = false;
			for (const world::ObjectHandle Target : Targets)
			{
				auto Access = Scene.Read();
				const components::CObjectMeshComponent &TargetMesh =
					Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Target));
				const auto TargetOverride =
					std::ranges::find_if(TargetMesh.GetMaterialOverrides(), [&](const components::MeshMaterialOverride &Value)
										 { return Value.MeshInstance == Instance.ID && Value.MaterialSlot == Slot.ID; });
				const resource::AssetHandle<resource::MaterialInterfaceAsset> TargetMaterial =
					TargetOverride == TargetMesh.GetMaterialOverrides().end() ? Slot.DefaultMaterial : TargetOverride->Material;
				const auto PinnedTargetMaterial = TargetMaterial.TryPin();
				if (!PinnedTargetMaterial)
					continue;
				const glm::vec4 TargetColor = PinnedTargetMaterial->GetFactors().BaseColor;
				if (glm::any(glm::notEqual(TargetColor, BaseColor)))
					MixedBaseColor = true;
			}
			if (MatchingColorEdit && State.MaterialColorEdit->Dirty)
				MixedBaseColor = false;
			if (FocusBaseColor)
			{
				ImGui::SetScrollHereY(0.5f);
				ImGui::SetKeyboardFocusHere();
				OpenColorEditPickerPopup("Base Color");
				FocusBaseColor = false;
			}
			const bool MaterialPersistencePending = Session.GetPrivateMaterialAssignmentService().HasPendingWork();
			if (MaterialPersistencePending && !MatchingColorEdit)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
				ImGui::BeginDisabled();
			}
			ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, MixedBaseColor);
			const bool BaseColorChanged = ImGui::ColorEdit4("Base Color", &BaseColor.x, ImGuiColorEditFlags_Float);
			const bool BaseColorEditFinished = ImGui::IsItemDeactivatedAfterEdit();
			ImGui::PopItemFlag();
			const bool BaseColorPickerOpen = IsColorEditPickerPopupOpen("Base Color");
			if (MaterialPersistencePending && !MatchingColorEdit)
			{
				ImGui::EndDisabled();
				ImGui::PopStyleVar();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("Waiting for the previous private material transaction to finish.");
			}
			if (BaseColorChanged)
			{
				try
				{
					if (!MatchingColorEdit)
					{
						if (State.MaterialColorEdit.has_value())
						{
							Session.GetPrivateMaterialAssignmentService().CancelBaseColorPreview(State.MaterialColorEdit->PreviewID);
							State.MaterialColorEdit.reset();
						}
						std::vector<material::PrivateMaterialTarget> &MaterialTargets = State.PrivateMaterialTargetsScratch;
						MaterialTargets.clear();
						MaterialTargets.reserve(Targets.size());
						for (const world::ObjectHandle Target : Targets)
							MaterialTargets.push_back({.Object = Target, .MeshInstance = Instance.ID, .MaterialSlot = Slot.ID});
						const util::UUID PreviewID = Session.GetPrivateMaterialAssignmentService().BeginBaseColorPreview(
							Session.GetDocument(), MaterialTargets, BaseColor);
						State.MaterialColorEdit.emplace();
						State.MaterialColorEdit->Object = Object;
						State.MaterialColorEdit->MeshInstance = Instance.ID;
						State.MaterialColorEdit->MaterialSlot = Slot.ID;
						State.MaterialColorEdit->PreviewID = PreviewID;
						State.MaterialColorEdit->Value = BaseColor;
						State.MaterialColorEdit->Dirty = true;
						State.MaterialColorEdit->PickerWasOpen = BaseColorPickerOpen;
					}
					else
					{
						Session.GetPrivateMaterialAssignmentService().UpdateBaseColorPreview(State.MaterialColorEdit->PreviewID, BaseColor);
						State.MaterialColorEdit->Value = BaseColor;
						State.MaterialColorEdit->Dirty = true;
					}
				}
				catch (const std::exception &Exception)
				{
					Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Material", Exception.what());
				}
			}
			const bool CurrentColorEdit = State.MaterialColorEdit.has_value() && State.MaterialColorEdit->Object == Object &&
										  State.MaterialColorEdit->MeshInstance == Instance.ID &&
										  State.MaterialColorEdit->MaterialSlot == Slot.ID;
			const bool PickerClosed = CurrentColorEdit && State.MaterialColorEdit->PickerWasOpen && !BaseColorPickerOpen;
			const bool InlineEditFinished =
				CurrentColorEdit && !State.MaterialColorEdit->PickerWasOpen && !BaseColorPickerOpen && BaseColorEditFinished;
			const bool CommitBaseColor = CurrentColorEdit && State.MaterialColorEdit->Dirty && (PickerClosed || InlineEditFinished);
			if (CurrentColorEdit && BaseColorPickerOpen)
				State.MaterialColorEdit->PickerWasOpen = true;
			if (CommitBaseColor)
			{
				const util::UUID PreviewID = State.MaterialColorEdit->PreviewID;
				State.MaterialColorEdit.reset();
				try
				{
					(void)Session.GetPrivateMaterialAssignmentService().CommitBaseColorPreview(PreviewID, Context.Scheduler);
				}
				catch (const std::exception &Exception)
				{
					Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Material", Exception.what());
				}
			}
			ImGui::PopID();
			ImGui::PopID();
		}
	}
}

[[nodiscard]] bool RenderBehaviorPropertyValue(components::BehaviorPropertyValue &Value)
{
	return std::visit(
		[](auto &Typed)
		{
			using ValueType = std::remove_cvref_t<decltype(Typed)>;
			if constexpr (std::same_as<ValueType, bool>)
				return ImGui::Checkbox("##Value", &Typed);
			else if constexpr (std::same_as<ValueType, int32>)
				return ImGui::InputScalar("##Value", ImGuiDataType_S32, &Typed);
			else if constexpr (std::same_as<ValueType, uint32>)
				return ImGui::InputScalar("##Value", ImGuiDataType_U32, &Typed);
			else if constexpr (std::same_as<ValueType, int64>)
				return ImGui::InputScalar("##Value", ImGuiDataType_S64, &Typed);
			else if constexpr (std::same_as<ValueType, uint64>)
				return ImGui::InputScalar("##Value", ImGuiDataType_U64, &Typed);
			else if constexpr (std::same_as<ValueType, float32>)
				return ImGui::DragFloat("##Value", &Typed, 0.05f);
			else if constexpr (std::same_as<ValueType, float64>)
				return ImGui::InputDouble("##Value", &Typed, 0.05);
			else if constexpr (std::same_as<ValueType, string>)
				return InputText("##Value", Typed);
			else if constexpr (std::same_as<ValueType, glm::vec2>)
				return ImGui::DragFloat2("##Value", &Typed.x, 0.05f);
			else if constexpr (std::same_as<ValueType, glm::vec3>)
				return ImGui::DragFloat3("##Value", &Typed.x, 0.05f);
			else if constexpr (std::same_as<ValueType, glm::vec4>)
				return ImGui::DragFloat4("##Value", &Typed.x, 0.05f);
			else if constexpr (std::same_as<ValueType, glm::quat>)
				return ImGui::DragFloat4("##Value", &Typed.x, 0.01f);
			else
			{
				ImGui::TextUnformatted(Typed.ToString().c_str());
				return false;
			}
		},
		Value);
}

void RenderBehaviorInstances(auto &State, EditorSession &Session, action::EditorActionContext &Context, const world::ObjectHandle Object)
{
	world::Scene &Scene = Session.GetDocument().GetScene();
	const auto Handle = Scene.GetComponent<components::CObjectBehaviorComponent>(Object);
	if (!Handle.IsValid())
		return;
	if (ImGui::Button("Add Behavior", ImVec2(-1.0f, 0.0f)))
		ImGui::OpenPopup("Add Registered Behavior");
	if (ImGui::BeginPopup("Add Registered Behavior"))
	{
		std::vector<runtime::behavior::BehaviorDescriptor> &Descriptors = State.BehaviorDescriptorsScratch;
		Session.GetBehaviorRegistry().SnapshotInto(Descriptors);
		std::ranges::sort(Descriptors, {}, &runtime::behavior::BehaviorDescriptor::Name);
		if (Descriptors.empty())
			ImGui::TextDisabled("No project behaviors are registered.");
		for (const runtime::behavior::BehaviorDescriptor &Descriptor : Descriptors)
		{
			if (!ImGui::MenuItem(Descriptor.Name.c_str()))
				continue;
			try
			{
				Session.GetDocument().Execute(std::make_unique<commands::AddBehaviorCommand>(Scene, Object, Descriptor));
			}
			catch (const std::exception &Exception)
			{
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
			}
		}
		ImGui::EndPopup();
	}

	std::vector<components::BehaviorInstance> &Behaviors = State.BehaviorInstancesScratch;
	Behaviors.clear();
	{
		auto Access = Scene.Read();
		Behaviors = Access.Resolve(Handle).GetBehaviors();
	}
	for (components::BehaviorInstance &Behavior : Behaviors)
	{
		ImGui::PushID(Behavior.InstanceID.ToString().c_str());
		bool Attached = true;
		const bool Expanded = ImGui::CollapsingHeader(Behavior.TypeName.c_str(), &Attached, ImGuiTreeNodeFlags_DefaultOpen);
		if (!Attached)
		{
			try
			{
				Session.GetDocument().Execute(std::make_unique<commands::RemoveBehaviorCommand>(Scene, Object, Behavior.InstanceID));
			}
			catch (const std::exception &Exception)
			{
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
			}
			ImGui::PopID();
			continue;
		}
		bool Changed = false;
		if (Expanded)
		{
			Changed |= ImGui::Checkbox("Enabled", &Behavior.Enabled);
			ImGui::SameLine();
			ImGui::TextDisabled("Schema %u", Behavior.SchemaVersion);
			std::vector<std::pair<const string *, components::BehaviorPropertyValue *>> &Properties = State.BehaviorPropertiesScratch;
			Properties.clear();
			Properties.reserve(Behavior.Properties.size());
			for (auto &[Name, Value] : Behavior.Properties)
				Properties.emplace_back(&Name, &Value);
			std::ranges::sort(Properties, [](const auto &Left, const auto &Right) { return *Left.first < *Right.first; });
			if (!Properties.empty() &&
				ImGui::BeginTable("BehaviorProperties", 2,
								  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.42f);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
				for (const auto &[Name, Value] : Properties)
				{
					ImGui::PushID(Name->c_str());
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(Name->c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::SetNextItemWidth(-1.0f);
					Changed |= RenderBehaviorPropertyValue(*Value);
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (!Behavior.Diagnostic.empty())
				ImGui::TextWrapped("%s", Behavior.Diagnostic.c_str());
		}
		if (Changed)
		{
			try
			{
				Session.GetDocument().Execute(std::make_unique<commands::EditBehaviorCommand>(Scene, Object, std::move(Behavior)));
			}
			catch (const std::exception &Exception)
			{
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
			}
		}
		ImGui::PopID();
	}
}

void RenderPropertiesPanel(auto &State, EditorSession &Session, action::EditorActionContext &Context)
{
	workspace::EditorPanelState &Panel = Session.GetWorkspace().GetPanel(workspace::EditorPanelID::Properties);
	if (!Panel.Open || Panel.Minimized)
		return;
	bool Open = Panel.Open;
	if (!ImGui::Begin("Properties", Panel.Closable ? &Open : nullptr))
	{
		ImGui::End();
		if (Open != Panel.Open)
			Session.GetWorkspace().SetOpen(Panel.ID, Open);
		return;
	}
	if (RenderPanelMinimizeControl(Session, Panel))
	{
		ImGui::End();
		return;
	}
	ImGui::SetNextItemWidth(-1.0f);
	(void)InputText("##PropertyFilter", State.PropertyFilter);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Filter properties");
	ImGui::Separator();

	const document::SelectionSet &Selection = Session.GetDocument().GetSelection();
	if (Selection.Empty())
		ImGui::TextDisabled("Select an object to inspect its components.");
	else
	{
		const world::ObjectHandle Object = Session.GetDocument().GetScene().FindObject(Selection.GetPrimary());
		if (Object.IsValid())
		{
			if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f)))
				ImGui::OpenPopup("Add Object Component");
			if (ImGui::BeginPopup("Add Object Component"))
			{
				const auto AddComponent = [&]<IsCObjectComponent ComponentType>(const char *Name)
				{
					std::vector<util::UUID> &Missing = State.MissingComponentObjectsScratch;
					Missing.clear();
					for (const util::UUID &ID : Selection.GetOrdered())
					{
						const world::ObjectHandle Target = Session.GetDocument().GetScene().FindObject(ID);
						if (Target.IsValid() && !Session.GetDocument().GetScene().GetComponent<ComponentType>(Target).IsValid())
							Missing.push_back(ID);
					}
					if (ImGui::MenuItem(Name, nullptr, false, !Missing.empty()))
					{
						try
						{
							commands::CommandHistory &History = Session.GetDocument().GetHistory();
							History.BeginTransaction("Add " + string(Name));
							for (const util::UUID &ID : Missing)
								History.Execute(
									std::make_unique<commands::AddComponentCommand>(Session.GetDocument(), ID, ComponentType::TypeID));
							History.CommitTransaction();
						}
						catch (const std::exception &Exception)
						{
							if (Session.GetDocument().GetHistory().HasOpenTransaction())
								Session.GetDocument().GetHistory().CancelTransaction();
							Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Properties", Exception.what());
						}
					}
				};
				AddComponent.template operator()<components::CObjectCameraComponent>("Camera");
				AddComponent.template operator()<components::CObjectBehaviorComponent>("Behavior");
				AddComponent.template operator()<components::CObjectDirectionalLightComponent>("Directional Light");
				AddComponent.template operator()<components::CObjectPointLightComponent>("Point Light");
				AddComponent.template operator()<components::CObjectSpotLightComponent>("Spot Light");
				ImGui::Separator();
				ImGui::TextDisabled("Drop a Model into the viewport to add a mesh.");
				ImGui::EndPopup();
			}
			RenderComponent<components::CObjectIdentityComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderComponent<components::CObjectTransformComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderComponent<components::CObjectHierarchyComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderComponent<components::CObjectCameraComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderComponent<components::CObjectMeshComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderMeshMaterials(State, Session, Context, Object);
			RenderComponent<components::CObjectAnimationComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderComponent<components::CObjectBehaviorComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderBehaviorInstances(State, Session, Context, Object);
			RenderComponent<components::CObjectDirectionalLightComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderComponent<components::CObjectPointLightComponent>(State, Session, Context, Object, State.PropertyFilter);
			RenderComponent<components::CObjectSpotLightComponent>(State, Session, Context, Object, State.PropertyFilter);
		}
	}
	ImGui::End();
	if (Open != Panel.Open)
		Session.GetWorkspace().SetOpen(Panel.ID, Open);
}

void CreateMeshObjectFromContent(EditorSession &Session, action::EditorActionContext &Context, const std::filesystem::path &RelativePath)
{
	const asset::AssetRegistrySnapshot &Snapshot = Session.GetAssetRegistry().GetSnapshot();
	const auto Entry = std::ranges::find(Snapshot.Entries, RelativePath, &asset::ContentEntry::RelativePath);
	if (Entry == Snapshot.Entries.end() || Entry->AssetType != resource::AssetType::Model)
		throw std::invalid_argument("Only a registered model asset can create a mesh object");
	resource::AssetHandle<resource::ModelAsset> Model =
		Session.GetProject().GetAssetManager().GetAssetByID<resource::ModelAsset>(Entry->ID);
	Session.GetDocument().Execute(
		std::make_unique<commands::CreateMeshObjectCommand>(Session.GetDocument(), RelativePath.stem().string(), std::move(Model)));
	Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Information, "Explorer",
								"Created mesh object from '" + RelativePath.generic_string() + "'");
}

void RenderExplorerPanel(auto &State, EditorSession &Session, action::EditorActionRegistry &Actions, action::EditorActionContext &Context)
{
	const auto RenderRegisteredMenuItem = [&Actions, &Context](const action::EditorActionID ID, const char *Shortcut)
	{
		const action::EditorActionDescriptor *Descriptor = Actions.Find(ID);
		if (Descriptor == nullptr)
			return false;
		const bool Enabled = Actions.CanExecute(ID, Context);
		const bool Activated = ImGui::MenuItem(Descriptor->DisplayName.c_str(), Shortcut, false, Enabled);
		if (!Enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			const string DisabledReason = Actions.GetDisabledReason(ID, Context);
			ImGui::SetTooltip("%s", DisabledReason.c_str());
		}
		return Activated;
	};
	workspace::EditorPanelState &Panel = Session.GetWorkspace().GetPanel(workspace::EditorPanelID::Explorer);
	if (!Panel.Open || Panel.Minimized)
		return;
	bool Open = Panel.Open;
	if (!ImGui::Begin("Explorer", Panel.Closable ? &Open : nullptr))
	{
		ImGui::End();
		if (Open != Panel.Open)
			Session.GetWorkspace().SetOpen(Panel.ID, Open);
		return;
	}
	if (RenderPanelMinimizeControl(Session, Panel))
	{
		ImGui::End();
		return;
	}
	string &Filter = State.HierarchyFilterScratch;
	Filter.assign(Session.GetHierarchyFilter());
	ImGui::SetNextItemWidth(-1.0f);
	if (InputText("##ExplorerSearch", Filter))
		Session.SetHierarchyFilter(std::move(Filter));
	ImGui::Separator();
	const auto ExecuteCommand = [&](commands::EditorCommandPtr Command)
	{
		try
		{
			Session.GetDocument().Execute(std::move(Command));
		}
		catch (const std::exception &Exception)
		{
			Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Explorer", Exception.what());
		}
	};
	const auto RenderPrimitiveCreationMenu = [&](const util::UUID Parent)
	{
		if (!ImGui::BeginMenu("Create Primitive", Session.GetPlaySession().GetState() == play::PlaySessionState::Stopped))
			return;
		for (const auto [Shape, Name] :
			 {std::pair{asset::PrimitiveShape::Box, "Box"}, std::pair{asset::PrimitiveShape::Sphere, "Sphere"},
			  std::pair{asset::PrimitiveShape::Capsule, "Capsule"}, std::pair{asset::PrimitiveShape::Cylinder, "Cylinder"},
			  std::pair{asset::PrimitiveShape::Cone, "Cone"}, std::pair{asset::PrimitiveShape::Plane, "Plane"}})
		{
			if (!ImGui::MenuItem(Name))
				continue;
			try
			{
				Session.CreatePrimitive(Shape, Parent);
			}
			catch (const std::exception &Exception)
			{
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Explorer", Exception.what());
			}
		}
		ImGui::EndMenu();
	};
	const auto EditSelectedIdentityProperty = [&](const string_view PropertyName, reflection::PropertyValue Value)
	{
		const std::optional<reflection::TypeDescriptor> Descriptor =
			Session.GetReflection().Find("components." + string(components::CObjectIdentityComponent::ComponentName));
		if (!Descriptor.has_value())
			throw std::logic_error("Identity reflection descriptor is unavailable");
		const auto Property = std::ranges::find(Descriptor->Properties, PropertyName, &reflection::PropertyDescriptor::Name);
		if (Property == Descriptor->Properties.end() || !Property->Write)
			throw std::logic_error("Identity property is not writable: " + string(PropertyName));

		commands::CommandHistory &History = Session.GetDocument().GetHistory();
		History.BeginTransaction("Edit object " + Property->DisplayName);
		try
		{
			for (const util::UUID &ID : Session.GetDocument().GetSelection().GetOrdered())
			{
				const world::ObjectHandle Object = Session.GetDocument().GetScene().FindObject(ID);
				if (!Object.IsValid())
					throw std::out_of_range("Selected object no longer exists");
				History.Execute(commands::PropertyEditCommand::Create(Session.GetDocument().GetScene(), Object,
																	  components::CObjectIdentityComponent::TypeID, *Property, Value,
																	  &Session.GetProject().GetAssetManager()));
			}
			History.CommitTransaction();
		}
		catch (...)
		{
			if (History.HasOpenTransaction())
				History.CancelTransaction();
			throw;
		}
	};

	const std::vector<hierarchy::SceneHierarchyRow> &Rows = Session.GetHierarchy().Rows;
	std::vector<uint32> &OpenDepths = State.ExplorerOpenDepthsScratch;
	OpenDepths.clear();
	for (usize RowIndex = 0; RowIndex < Rows.size(); ++RowIndex)
	{
		const hierarchy::SceneHierarchyRow &Row = Rows[RowIndex];
		while (!OpenDepths.empty() && OpenDepths.back() >= Row.Depth)
		{
			ImGui::TreePop();
			OpenDepths.pop_back();
		}
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (Row.ChildCount == 0)
			Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if (Session.GetDocument().GetSelection().Contains(Row.PersistentID))
			Flags |= ImGuiTreeNodeFlags_Selected;
		if (!State.ExpandedObjects.contains(Row.PersistentID))
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		const bool Expanded =
			ImGui::TreeNodeEx(reinterpret_cast<const void *>(static_cast<uintptr_t>(Row.Object.Slot + 1U)), Flags, "%s", Row.Name.c_str());
		const ImVec2 RowMinimum = ImGui::GetItemRectMin();
		const ImVec2 RowMaximum = ImGui::GetItemRectMax();
		State.ExpandedObjects[Row.PersistentID] = Expanded;
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
		{
			const ImGuiIO &IO = ImGui::GetIO();
			if (IO.KeyShift && State.ExplorerSelectionAnchor.IsValid())
			{
				const auto Anchor = std::ranges::find(Rows, State.ExplorerSelectionAnchor, &hierarchy::SceneHierarchyRow::PersistentID);
				if (Anchor != Rows.end())
				{
					const usize AnchorIndex = static_cast<usize>(std::distance(Rows.begin(), Anchor));
					const usize First = std::min(AnchorIndex, RowIndex);
					const usize Last = std::max(AnchorIndex, RowIndex);
					if (!IO.KeyCtrl)
						Session.GetDocument().GetSelection().Clear();
					for (usize SelectionRow = First; SelectionRow <= Last; ++SelectionRow)
						Session.GetDocument().GetSelection().Add(Rows[SelectionRow].PersistentID);
				}
				else
				{
					Session.GetDocument().GetSelection().SelectOnly(Row.PersistentID);
					State.ExplorerSelectionAnchor = Row.PersistentID;
				}
			}
			else if (IO.KeyCtrl)
			{
				Session.GetDocument().GetSelection().Toggle(Row.PersistentID);
				State.ExplorerSelectionAnchor = Row.PersistentID;
			}
			else
			{
				Session.GetDocument().GetSelection().SelectOnly(Row.PersistentID);
				State.ExplorerSelectionAnchor = Row.PersistentID;
			}
		}
		if (ImGui::BeginDragDropSource())
		{
			const string ID = Row.PersistentID.ToString();
			ImGui::SetDragDropPayload("SCENE_OBJECT_ID", ID.c_str(), ID.size() + 1U);
			ImGui::TextUnformatted(Row.Name.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload *Payload = ImGui::AcceptDragDropPayload(
					"SCENE_OBJECT_ID", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
			{
				const float32 Height = std::max(RowMaximum.y - RowMinimum.y, 1.0f);
				const float32 RelativeY = std::clamp((ImGui::GetMousePos().y - RowMinimum.y) / Height, 0.0f, 1.0f);
				const bool InsertBefore = RelativeY < 0.25f;
				const bool InsertAfter = RelativeY > 0.75f;
				const ImU32 Indicator = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
				if (InsertBefore || InsertAfter)
				{
					const float32 Y = InsertBefore ? RowMinimum.y : RowMaximum.y;
					ImGui::GetWindowDrawList()->AddLine(ImVec2(RowMinimum.x, Y), ImVec2(RowMaximum.x, Y), Indicator, 2.0f);
				}
				else
				{
					ImGui::GetWindowDrawList()->AddRect(RowMinimum, RowMaximum, Indicator, 3.0f, 0, 2.0f);
				}
				if (Payload->IsDelivery())
				{
					const string ID(static_cast<const char *>(Payload->Data),
									(static_cast<const char *>(Payload->Data) + Payload->DataSize) - 1);
					const util::UUID Child = util::UUID::Parse(ID);
					if (Child != Row.PersistentID)
					{
						util::UUID Parent = Row.PersistentID;
						uint32 InsertionOrder = ~uint32{0};
						if (InsertBefore || InsertAfter)
						{
							Parent = Row.ParentRow == hierarchy::InvalidHierarchyRow ? util::UUID{} : Rows[Row.ParentRow].PersistentID;
							InsertionOrder = Row.SiblingOrder + (InsertAfter ? 1U : 0U);
							const auto Source = std::ranges::find(Rows, Child, &hierarchy::SceneHierarchyRow::PersistentID);
							if (Source != Rows.end())
							{
								const util::UUID SourceParent = Source->ParentRow == hierarchy::InvalidHierarchyRow
																	? util::UUID{}
																	: Rows[Source->ParentRow].PersistentID;
								if (SourceParent == Parent && Source->SiblingOrder < InsertionOrder)
									--InsertionOrder;
							}
						}
						ExecuteCommand(
							std::make_unique<commands::ReparentObjectCommand>(Session.GetDocument(), Child, Parent, InsertionOrder));
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
		if (ImGui::BeginPopupContextItem())
		{
			if (!Session.GetDocument().GetSelection().Contains(Row.PersistentID))
				Session.GetDocument().GetSelection().SelectOnly(Row.PersistentID);
			if (ImGui::MenuItem("Create Child"))
				ExecuteCommand(std::make_unique<commands::CreateObjectCommand>(Session.GetDocument(), "Object", Row.PersistentID));
			RenderPrimitiveCreationMenu(Row.PersistentID);
			if (ImGui::MenuItem("Rename"))
			{
				State.ExplorerRenameObject = Row.PersistentID;
				State.ExplorerRenameValue = Row.Name;
				State.ExplorerRenameRequested = true;
			}
			if (RenderRegisteredMenuItem(action::IDs::CopyObjects, "Ctrl+C"))
				(void)Actions.Invoke(action::IDs::CopyObjects, Context);
			if (RenderRegisteredMenuItem(action::IDs::PasteObjects, "Ctrl+V"))
				(void)Actions.Invoke(action::IDs::PasteObjects, Context);
			if (RenderRegisteredMenuItem(action::IDs::DuplicateObjects, "Ctrl+D"))
				(void)Actions.Invoke(action::IDs::DuplicateObjects, Context);
			if (RenderRegisteredMenuItem(action::IDs::GroupObjects, "Ctrl+G"))
				(void)Actions.Invoke(action::IDs::GroupObjects, Context);
			if (ImGui::MenuItem("Move to Root"))
				ExecuteCommand(std::make_unique<commands::ReparentObjectCommand>(Session.GetDocument(), Row.PersistentID, util::UUID{}));
			ImGui::Separator();
			try
			{
				if (ImGui::MenuItem("Enabled", nullptr, Row.Enabled))
					EditSelectedIdentityProperty("Enabled", !Row.Enabled);
				if (ImGui::MenuItem("Visible in Editor", nullptr, Row.EditorVisible))
					EditSelectedIdentityProperty("EditorVisible", !Row.EditorVisible);
				if (ImGui::MenuItem("Locked", nullptr, Row.Locked))
					EditSelectedIdentityProperty("Locked", !Row.Locked);
				if (ImGui::BeginMenu("Mobility"))
				{
					for (const auto [Mobility, Name] : {std::pair{components::ObjectMobility::Static, "Static"},
														std::pair{components::ObjectMobility::Stationary, "Stationary"},
														std::pair{components::ObjectMobility::Movable, "Movable"}})
					{
						if (ImGui::MenuItem(Name, nullptr, Row.Mobility == Mobility))
							EditSelectedIdentityProperty("Mobility", static_cast<uint32>(Mobility));
					}
					ImGui::EndMenu();
				}
			}
			catch (const std::exception &Exception)
			{
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Explorer", Exception.what());
			}
			ImGui::Separator();
			if (RenderRegisteredMenuItem(action::IDs::DeleteObjects, "Delete"))
				(void)Actions.Invoke(action::IDs::DeleteObjects, Context);
			ImGui::EndPopup();
		}
		if (Row.ChildCount != 0 && Expanded)
			OpenDepths.push_back(Row.Depth);
		else if (Row.ChildCount != 0 && !Expanded)
		{
			while (RowIndex + 1U < Rows.size() && Rows[RowIndex + 1U].Depth > Row.Depth)
				++RowIndex;
		}
	}
	while (!OpenDepths.empty())
	{
		ImGui::TreePop();
		OpenDepths.pop_back();
	}
	if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
	{
		Session.GetDocument().GetSelection().Clear();
		State.ExplorerSelectionAnchor = {};
	}
	if (ImGui::BeginPopupContextWindow("ExplorerContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		if (ImGui::MenuItem("Create Object"))
			ExecuteCommand(std::make_unique<commands::CreateObjectCommand>(Session.GetDocument(), "Object"));
		RenderPrimitiveCreationMenu({});
		if (RenderRegisteredMenuItem(action::IDs::PasteObjects, "Ctrl+V"))
			(void)Actions.Invoke(action::IDs::PasteObjects, Context);
		ImGui::EndPopup();
	}
	if (State.ExplorerRenameRequested)
	{
		ImGui::OpenPopup("Rename Scene Object");
		State.ExplorerRenameRequested = false;
	}
	if (ImGui::BeginPopupModal("Rename Scene Object", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::SetNextItemWidth(360.0f);
		(void)InputText("Name", State.ExplorerRenameValue);
		const bool CanSubmit = !State.ExplorerRenameValue.empty();
		if (!CanSubmit)
			ImGui::BeginDisabled();
		if (ImGui::Button("Rename", ImVec2(110.0f, 0.0f)))
		{
			ExecuteCommand(std::make_unique<commands::RenameObjectCommand>(Session.GetDocument(), State.ExplorerRenameObject,
																		   State.ExplorerRenameValue));
			ImGui::CloseCurrentPopup();
		}
		if (!CanSubmit)
			ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::End();
	if (Open != Panel.Open)
		Session.GetWorkspace().SetOpen(Panel.ID, Open);
}

void RenderAssetBrowser(auto &State, EditorSession &Session, action::EditorActionRegistry &Actions, action::EditorActionContext &Context)
{
	Session.TickAssetThumbnails(Context.Scheduler);
	workspace::EditorPanelState &Panel = Session.GetWorkspace().GetPanel(workspace::EditorPanelID::AssetBrowser);
	if (!Panel.Open || Panel.Minimized)
		return;
	bool Open = Panel.Open;
	if (!ImGui::Begin("Assets", Panel.Closable ? &Open : nullptr))
	{
		ImGui::End();
		if (Open != Panel.Open)
			Session.GetWorkspace().SetOpen(Panel.ID, Open);
		return;
	}
	if (RenderPanelMinimizeControl(Session, Panel))
	{
		ImGui::End();
		return;
	}
	asset::AssetContentService &ContentService = Session.GetAssetContentService();
	asset::AssetImportService &Import = Session.GetAssetImportService();
	const auto QueueOperation = [&](asset::AssetContentRequest Request)
	{
		try
		{
			ContentService.Queue(std::move(Request), Context.Scheduler);
		}
		catch (const std::exception &Exception)
		{
			Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Content", Exception.what());
		}
	};

	if (ImGui::Button(State.TrashView ? "Content" : "Trash"))
	{
		State.TrashView = !State.TrashView;
		State.TrashLoaded = false;
		State.SelectedContentPath.clear();
	}
	ImGui::SameLine();
	if (!State.TrashView)
	{
		if (ImGui::Button("Game"))
			State.ContentDirectory.clear();
		std::filesystem::path Breadcrumb;
		for (const std::filesystem::path &Part : State.ContentDirectory)
		{
			Breadcrumb /= Part;
			ImGui::SameLine();
			ImGui::TextDisabled("/");
			ImGui::SameLine();
			State.UITextScratch = Part.string();
			State.UITextScratch += "##Breadcrumb";
			State.UITextScratch += Breadcrumb.generic_string();
			if (ImGui::SmallButton(State.UITextScratch.c_str()))
				State.ContentDirectory = Breadcrumb;
		}
		ImGui::SameLine();
		if (ImGui::Button("New"))
			ImGui::OpenPopup("Create Content");
		if (ImGui::BeginPopup("Create Content"))
		{
			if (ImGui::MenuItem("Folder", nullptr, false, !ContentService.IsBusy()))
			{
				State.ActiveContentDialog = ContentDialog::NewFolder;
				State.ContentDialogValue = (State.ContentDirectory / "New Folder").generic_string();
				State.ContentDialogRequested = true;
			}
			if (ImGui::MenuItem("Material", nullptr, false, !ContentService.IsBusy()))
			{
				State.ActiveContentDialog = ContentDialog::NewMaterial;
				State.ContentDialogValue = (State.ContentDirectory / "New Material.material").generic_string();
				State.ContentDialogRequested = true;
			}

			const asset::ContentEntry *SelectedParent = nullptr;
			for (const asset::ContentEntry &Entry : Session.GetAssetRegistry().GetSnapshot().Entries)
			{
				if (Entry.RelativePath == State.SelectedContentPath &&
					(Entry.AssetType == resource::AssetType::Material || Entry.AssetType == resource::AssetType::MaterialInstance))
				{
					SelectedParent = &Entry;
					break;
				}
			}
			if (ImGui::MenuItem("Material Instance", nullptr, false, !ContentService.IsBusy() && SelectedParent != nullptr))
			{
				State.ActiveContentDialog = ContentDialog::NewMaterialInstance;
				State.ContentDialogValue = (State.ContentDirectory / "New Material Instance.materialinstance").generic_string();
				State.ContentParentAssetID = SelectedParent->ID;
				State.ContentParentAssetType = *SelectedParent->AssetType;
				State.ContentDialogRequested = true;
			}
			if (SelectedParent == nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Select a material or material instance to use as the parent.");
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(State.FavoritesOnly ? "All Assets" : "Favorites"))
			State.FavoritesOnly = !State.FavoritesOnly;
		ImGui::SameLine();
		constexpr std::array ContentTypeNames{"All Types",		"Texture 2D",	   "Material",		   "Material Instance",
											  "Model",			"Static Mesh",	   "Skeletal Mesh",	   "Skeleton",
											  "Animation Clip", "Animation Graph", "Retarget Profile", "Shader Source"};
		const int32 TypeNameIndex = State.ContentTypeFilter < 0 ? 0 : State.ContentTypeFilter + 1;
		ImGui::SetNextItemWidth(125.0f);
		if (ImGui::BeginCombo("##ContentTypeFilter", ContentTypeNames.at(static_cast<usize>(TypeNameIndex))))
		{
			for (int32 Index = -1; Index < static_cast<int32>(resource::AssetType::Count); ++Index)
			{
				const bool Selected = State.ContentTypeFilter == Index;
				if (ImGui::Selectable(ContentTypeNames.at(static_cast<usize>(Index + 1)), Selected))
					State.ContentTypeFilter = Index;
				if (Selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 92.0f));
		(void)InputText("##AssetSearch", State.AssetFilter);
		ImGui::SameLine();
		if (ImGui::Button(State.AssetGridView ? "List" : "Grid"))
			State.AssetGridView = !State.AssetGridView;
	}
	else
	{
		ImGui::SameLine();
		ImGui::TextUnformatted("Project Trash");
		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
			State.TrashLoaded = false;
	}

	if (Import.IsBusy())
	{
		const asset::AssetImportProgress Progress = Import.GetProgress();
		ImGui::ProgressBar(Progress.GetFraction(), ImVec2(-90.0f, 0.0f));
		ImGui::SameLine();
		if (Actions.CanExecute(action::IDs::CancelAssetImport, Context) && ImGui::Button("Cancel"))
			(void)Actions.Invoke(action::IDs::CancelAssetImport, Context);
	}
	else if (Import.GetResult().has_value())
	{
		ImGui::TextDisabled("%s", Import.GetResult()->Diagnostic.c_str());
	}
	if (ContentService.IsBusy())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("Content operation in progress...");
		ImGui::SameLine();
		if (ImGui::SmallButton("Cancel##ContentOperation"))
			ContentService.Cancel();
	}
	else if (ContentService.GetResult().has_value())
	{
		ImGui::TextDisabled("%s", ContentService.GetResult()->Diagnostic.c_str());
	}
	if (Session.GetAssetReloadService().IsBusy())
		ImGui::TextDisabled("Reimporting asset...");
	else if (Session.GetAssetReloadService().GetResult().has_value())
		ImGui::TextDisabled("%s", Session.GetAssetReloadService().GetResult()->Diagnostic.c_str());
	ImGui::Separator();

	if (State.TrashView)
	{
		if (!State.TrashLoaded)
		{
			try
			{
				State.TrashEntries = ContentService.ScanTrash();
			}
			catch (const std::exception &Exception)
			{
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Content", Exception.what());
			}
			State.TrashLoaded = true;
		}
		if (State.TrashEntries.empty())
			ImGui::TextDisabled("Project Trash is empty.");
		if (ImGui::BeginTable("TrashTable", 3,
							  ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Original Path", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableHeadersRow();
			for (const asset::TrashedContentEntry &Entry : State.TrashEntries)
			{
				ImGui::PushID(&Entry);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				State.UITextScratch = Entry.OriginalPath.filename().string();
				ImGui::TextUnformatted(State.UITextScratch.c_str());
				ImGui::TableSetColumnIndex(1);
				State.UITextScratch = Entry.OriginalPath.generic_string();
				ImGui::TextDisabled("%s", State.UITextScratch.c_str());
				ImGui::TableSetColumnIndex(2);
				if (!ContentService.IsBusy() && ImGui::SmallButton("Restore"))
				{
					QueueOperation({.Operation = asset::AssetContentOperation::Restore, .TrashEntryID = Entry.ID});
					State.TrashLoaded = false;
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
	else
	{
		const asset::AssetRegistrySnapshot &Snapshot = Session.GetAssetRegistry().GetSnapshot();
		std::unordered_map<resource::AssetID, uint64> &LiveAssetIDs = State.LiveAssetIDsScratch;
		++State.LiveAssetIDGeneration;
		if (State.LiveAssetIDGeneration == 0)
		{
			LiveAssetIDs.clear();
			State.LiveAssetIDGeneration = 1;
		}
		const uint64 LiveAssetIDGeneration = State.LiveAssetIDGeneration;
		LiveAssetIDs.reserve(Snapshot.Entries.size());
		for (const asset::ContentEntry &Entry : Snapshot.Entries)
		{
			if (Entry.Kind == asset::ContentEntryKind::Asset)
				LiveAssetIDs.insert_or_assign(Entry.ID, LiveAssetIDGeneration);
		}
		PruneThumbnailTextures(State, LiveAssetIDs, LiveAssetIDGeneration);
		if (!Snapshot.Diagnostics.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "%zu registry diagnostic%s", Snapshot.Diagnostics.size(),
							   Snapshot.Diagnostics.size() == 1 ? "" : "s");
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				for (const string &Diagnostic : Snapshot.Diagnostics)
					ImGui::BulletText("%s", Diagnostic.c_str());
				ImGui::EndTooltip();
			}
		}
		if (ImGui::BeginTable("ContentBrowserSplit", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 190.0f);
			ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::BeginChild("ContentFolders");
			const bool RootSelected = State.ContentDirectory.empty();
			if (ImGui::Selectable("/Game", RootSelected))
				State.ContentDirectory.clear();
			for (const asset::ContentEntry &Entry : Snapshot.Entries)
			{
				if (Entry.Kind != asset::ContentEntryKind::Directory || Entry.Hidden)
					continue;
				const usize Depth = static_cast<usize>(std::distance(Entry.RelativePath.begin(), Entry.RelativePath.end()));
				ImGui::Indent(static_cast<float32>(Depth) * 12.0f);
				const bool Selected = Entry.RelativePath == State.ContentDirectory;
				if (ImGui::Selectable(Entry.DisplayName.c_str(), Selected))
					State.ContentDirectory = Entry.RelativePath;
				ImGui::Unindent(static_cast<float32>(Depth) * 12.0f);
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *Payload = ImGui::AcceptDragDropPayload("ASSET_CONTENT_PATH"))
					{
						const string Source(static_cast<const char *>(Payload->Data),
											(static_cast<const char *>(Payload->Data) + Payload->DataSize) - 1);
						const std::filesystem::path SourcePath(Source);
						QueueOperation({.Operation = asset::AssetContentOperation::Move,
										.Source = SourcePath,
										.Destination = Entry.RelativePath / SourcePath.filename()});
					}
					ImGui::EndDragDropTarget();
				}
			}
			ImGui::EndChild();
			ImGui::TableSetColumnIndex(1);
			ImGui::BeginChild("ContentEntries");
			std::vector<const asset::ContentEntry *> &Entries = State.ContentEntriesScratch;
			Entries.clear();
			if (State.AssetFilter.empty())
			{
				for (const asset::ContentEntry &Entry : Snapshot.Entries)
				{
					if (!Entry.Hidden && Entry.ParentPath == State.ContentDirectory)
						Entries.push_back(&Entry);
				}
			}
			else
			{
				Snapshot.SearchInto(State.AssetFilter, Entries);
				std::erase_if(Entries, [](const asset::ContentEntry *Entry) { return Entry->Hidden; });
			}
			std::erase_if(Entries,
						  [&State](const asset::ContentEntry *Entry)
						  {
							  const bool Favorite = State.FavoriteAssetIDs.contains(Entry->ID);
							  const bool TypeMatches =
								  State.ContentTypeFilter < 0 ||
								  (Entry->AssetType.has_value() && static_cast<int32>(*Entry->AssetType) == State.ContentTypeFilter);
							  return (State.FavoritesOnly && !Favorite) || !TypeMatches;
						  });

			const auto RenderEntryInteraction = [&](const asset::ContentEntry &Entry)
			{
				if (ImGui::IsItemClicked())
					State.SelectedContentPath = Entry.RelativePath;
				if (Entry.Kind == asset::ContentEntryKind::Directory && ImGui::IsItemHovered() &&
					ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					State.ContentDirectory = Entry.RelativePath;
					State.AssetFilter.clear();
				}
				else if ((Entry.AssetType == resource::AssetType::Material || Entry.AssetType == resource::AssetType::MaterialInstance) &&
						 ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					if (State.MaterialSession.has_value() && State.MaterialSession->IsDirty())
					{
						Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Warning, "Material",
													"Save or revert the open material before opening another asset");
					}
					else
					{
						try
						{
							State.MaterialSession = material::MaterialEditorSession::Open(
								Session.GetProject().ResolveContentPath(Entry.RelativePath), Session.GetProject().GetAssetManager());
							Session.GetWorkspace().SetOpen(workspace::EditorPanelID::MaterialEditor, true);
						}
						catch (const std::exception &Exception)
						{
							Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Material", Exception.what());
						}
					}
				}
				if (ImGui::BeginDragDropSource())
				{
					State.UITextScratch = Entry.RelativePath.generic_string();
					ImGui::SetDragDropPayload("ASSET_CONTENT_PATH", State.UITextScratch.c_str(), State.UITextScratch.size() + 1U);
					ImGui::TextUnformatted(Entry.DisplayName.c_str());
					ImGui::EndDragDropSource();
				}
				if (Entry.Kind == asset::ContentEntryKind::Directory && ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *Payload = ImGui::AcceptDragDropPayload("ASSET_CONTENT_PATH"))
					{
						const string Source(static_cast<const char *>(Payload->Data),
											(static_cast<const char *>(Payload->Data) + Payload->DataSize) - 1);
						const std::filesystem::path SourcePath(Source);
						if (SourcePath != Entry.RelativePath)
							QueueOperation({.Operation = asset::AssetContentOperation::Move,
											.Source = SourcePath,
											.Destination = Entry.RelativePath / SourcePath.filename()});
					}
					ImGui::EndDragDropTarget();
				}
				if (ImGui::BeginPopupContextItem())
				{
					State.SelectedContentPath = Entry.RelativePath;
					if (Entry.AssetType == resource::AssetType::Model && ImGui::MenuItem("Add to Scene"))
					{
						try
						{
							CreateMeshObjectFromContent(Session, Context, Entry.RelativePath);
						}
						catch (const std::exception &Exception)
						{
							Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Explorer", Exception.what());
						}
					}
					const bool Favorite = State.FavoriteAssetIDs.contains(Entry.ID);
					if (ImGui::MenuItem(Favorite ? "Remove from Favorites" : "Add to Favorites"))
					{
						if (Favorite)
							State.FavoriteAssetIDs.erase(Entry.ID);
						else
							State.FavoriteAssetIDs.insert(Entry.ID);
						try
						{
							std::vector<resource::AssetID> &FavoriteAssetIDs = State.FavoriteAssetIDsScratch;
							FavoriteAssetIDs.assign(State.FavoriteAssetIDs.begin(), State.FavoriteAssetIDs.end());
							State.ContentBrowserStore->Save(FavoriteAssetIDs);
						}
						catch (const std::exception &Exception)
						{
							Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "ContentBrowser", Exception.what());
						}
					}
					if (Entry.Kind == asset::ContentEntryKind::Asset && ImGui::MenuItem("Dependencies..."))
					{
						State.InspectedAssetID = Entry.ID;
						State.DependencyInspectorRequested = true;
					}
					if (Entry.Kind == asset::ContentEntryKind::Asset &&
						ImGui::MenuItem("Reimport / Reload", nullptr, false, !Session.GetAssetReloadService().IsBusy()))
					{
						try
						{
							Session.GetAssetReloadService().Begin(Context.Scheduler, Entry.ID, Entry.MetadataPath);
						}
						catch (const std::exception &Exception)
						{
							Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "AssetReload", Exception.what());
						}
					}
					if (ImGui::MenuItem("Rename", nullptr, false, !ContentService.IsBusy()))
					{
						State.ActiveContentDialog = ContentDialog::Rename;
						State.ContentDialogValue = Entry.DisplayName;
						State.ContentDialogRequested = true;
					}
					if (ImGui::MenuItem("Move...", nullptr, false, !ContentService.IsBusy()))
					{
						State.ActiveContentDialog = ContentDialog::Move;
						State.ContentDialogValue = Entry.RelativePath.generic_string();
						State.ContentDialogRequested = true;
					}
					if (ImGui::MenuItem("Duplicate...", nullptr, false, !ContentService.IsBusy()))
					{
						State.ActiveContentDialog = ContentDialog::Duplicate;
						const std::filesystem::path Path = Entry.RelativePath;
						State.ContentDialogValue =
							(Path.parent_path() / (Path.stem().string() + " Copy" + Path.extension().string())).generic_string();
						State.ContentDialogRequested = true;
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Move to Trash", nullptr, false, !ContentService.IsBusy()))
					{
						State.ActiveContentDialog = ContentDialog::Trash;
						State.ContentDialogValue.clear();
						State.ContentDialogRequested = true;
					}
					ImGui::EndPopup();
				}
			};

			if (State.AssetGridView)
			{
				const float32 CardWidth = 132.0f;
				const ImVec2 ThumbnailSize(96.0f, 96.0f);
				const int32 ColumnCount = std::max<int32>(1, static_cast<int32>(ImGui::GetContentRegionAvail().x / CardWidth));
				if (ImGui::BeginTable("AssetGrid", ColumnCount))
				{
					for (const asset::ContentEntry *Entry : Entries)
					{
						ImGui::TableNextColumn();
						State.UITextScratch = Entry->RelativePath.generic_string();
						ImGui::PushID(State.UITextScratch.c_str());
						const bool Selected = State.SelectedContentPath == Entry->RelativePath;
						ImGui::BeginGroup();
						const float32 HorizontalInset = std::max(0.0f, (CardWidth - 8.0f - ThumbnailSize.x) * 0.5f);
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + HorizontalInset);
						ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, Selected ? 2.0f : 1.0f);
						ImGui::PushStyleColor(ImGuiCol_Border, Selected ? ImGui::GetStyleColorVec4(ImGuiCol_SliderGrabActive)
																		: ImGui::GetStyleColorVec4(ImGuiCol_Border));
						string &ThumbnailDiagnostic = State.ThumbnailDiagnosticScratch;
						ThumbnailDiagnostic.clear();
						ImTextureData *Thumbnail = nullptr;
						try
						{
							if (ImGui::IsRectVisible(ThumbnailSize))
								Thumbnail = ResolveThumbnailTexture(State, Session, *Entry, ThumbnailDiagnostic);
						}
						catch (const std::exception &Exception)
						{
							ThumbnailDiagnostic = Exception.what();
							Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "AssetThumbnail", Exception.what());
						}
						if (Thumbnail != nullptr)
						{
							(void)ImGui::ImageButton("##Thumbnail", Thumbnail->GetTexRef(), ThumbnailSize);
						}
						else
						{
							const char *Placeholder = Entry->Kind == asset::ContentEntryKind::Directory
														  ? "Folder"
														  : (ThumbnailDiagnostic.empty() ? "Loading..." : "Preview\nunavailable");
							(void)ImGui::Button(Placeholder, ThumbnailSize);
						}
						ImGui::PopStyleColor();
						ImGui::PopStyleVar();
						const bool ThumbnailHovered = ImGui::IsItemHovered();
						RenderEntryInteraction(*Entry);
						if (ThumbnailHovered && !ThumbnailDiagnostic.empty())
							ImGui::SetTooltip("%s", ThumbnailDiagnostic.c_str());
						ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + CardWidth - 8.0f);
						ImGui::TextUnformatted(Entry->DisplayName.c_str());
						ImGui::PopTextWrapPos();
						ImGui::EndGroup();
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
			}
			else if (ImGui::BeginTable("AssetTable", 3,
									   ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
										   ImGuiTableFlags_BordersInnerV))
			{
				ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
				ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableHeadersRow();
				for (const asset::ContentEntry *Entry : Entries)
				{
					State.UITextScratch = Entry->RelativePath.generic_string();
					ImGui::PushID(State.UITextScratch.c_str());
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					const bool Selected = State.SelectedContentPath == Entry->RelativePath;
					ImGui::Selectable(Entry->DisplayName.c_str(), Selected,
									  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
					RenderEntryInteraction(*Entry);
					ImGui::TableSetColumnIndex(1);
					ImGui::TextDisabled("%s", Entry->Kind == asset::ContentEntryKind::Directory ? "Folder" : Entry->Extension.c_str());
					ImGui::TableSetColumnIndex(2);
					State.UITextScratch = Entry->RelativePath.generic_string();
					ImGui::TextUnformatted(State.UITextScratch.c_str());
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}

		if (State.ContentDialogRequested)
		{
			ImGui::OpenPopup("Content Operation");
			State.ContentDialogRequested = false;
		}
		if (State.ActiveContentDialog != ContentDialog::None)
		{
			const bool CreateOperation = State.ActiveContentDialog == ContentDialog::NewFolder ||
										 State.ActiveContentDialog == ContentDialog::NewMaterial ||
										 State.ActiveContentDialog == ContentDialog::NewMaterialInstance;
			const char *Title = State.ActiveContentDialog == ContentDialog::Trash
									? "Move selected content to Project Trash?"
									: (CreateOperation ? "Create in project Content" : "Content destination");
			if (ImGui::BeginPopupModal("Content Operation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::TextUnformatted(Title);
				if (!CreateOperation)
					ImGui::TextDisabled("%s", State.SelectedContentPath.generic_string().c_str());
				else if (State.ActiveContentDialog == ContentDialog::NewMaterialInstance)
					ImGui::TextDisabled("Parent: %s", State.ContentParentAssetID.c_str());
				if (State.ActiveContentDialog != ContentDialog::Trash)
				{
					ImGui::SetNextItemWidth(420.0f);
					(void)InputText("##ContentDestination", State.ContentDialogValue);
				}
				const bool CanSubmit =
					!ContentService.IsBusy() && (State.ActiveContentDialog == ContentDialog::Trash || !State.ContentDialogValue.empty());
				if (!CanSubmit)
					ImGui::BeginDisabled();
				if (ImGui::Button(State.ActiveContentDialog == ContentDialog::Trash ? "Move to Trash" : "Apply", ImVec2(120.0f, 0.0f)))
				{
					asset::AssetContentRequest Request{.Source = State.SelectedContentPath};
					switch (State.ActiveContentDialog)
					{
					case ContentDialog::NewFolder:
						Request.Operation = asset::AssetContentOperation::CreateFolder;
						Request.Source.clear();
						Request.Destination = State.ContentDialogValue;
						break;
					case ContentDialog::NewMaterial:
						Request.Operation = asset::AssetContentOperation::CreateMaterial;
						Request.Source.clear();
						Request.Destination = State.ContentDialogValue;
						break;
					case ContentDialog::NewMaterialInstance:
						Request.Operation = asset::AssetContentOperation::CreateMaterialInstance;
						Request.Source.clear();
						Request.Destination = State.ContentDialogValue;
						Request.ParentAssetID = State.ContentParentAssetID;
						Request.ParentAssetType = State.ContentParentAssetType;
						break;
					case ContentDialog::Rename:
						Request.Operation = asset::AssetContentOperation::Move;
						Request.Destination = State.SelectedContentPath.parent_path() / State.ContentDialogValue;
						break;
					case ContentDialog::Move:
						Request.Operation = asset::AssetContentOperation::Move;
						Request.Destination = State.ContentDialogValue;
						break;
					case ContentDialog::Duplicate:
						Request.Operation = asset::AssetContentOperation::Duplicate;
						Request.Destination = State.ContentDialogValue;
						break;
					case ContentDialog::Trash:
						Request.Operation = asset::AssetContentOperation::Trash;
						break;
					case ContentDialog::None:
						break;
					}
					QueueOperation(std::move(Request));
					State.ActiveContentDialog = ContentDialog::None;
					ImGui::CloseCurrentPopup();
				}
				if (!CanSubmit)
					ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)))
				{
					State.ActiveContentDialog = ContentDialog::None;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}
		if (State.DependencyInspectorRequested)
		{
			ImGui::OpenPopup("Asset Dependencies");
			State.DependencyInspectorRequested = false;
		}
		if (ImGui::BeginPopupModal("Asset Dependencies", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const asset::ContentEntry *Entry = Session.GetAssetRegistry().GetSnapshot().Find(State.InspectedAssetID);
			if (Entry == nullptr)
				ImGui::TextDisabled("The selected asset is no longer registered.");
			else
			{
				ImGui::TextUnformatted(Entry->DisplayName.c_str());
				ImGui::TextDisabled("%s", Entry->ID.c_str());
				ImGui::SeparatorText("Dependencies");
				if (Entry->Dependencies.empty())
					ImGui::TextDisabled("None");
				for (const resource::AssetID &Dependency : Entry->Dependencies)
				{
					const asset::ContentEntry *Resolved = Session.GetAssetRegistry().GetSnapshot().Find(Dependency);
					ImGui::BulletText("%s", Resolved == nullptr ? Dependency.c_str() : Resolved->VirtualPath.c_str());
				}
				ImGui::SeparatorText("Referenced By");
				if (Entry->ReverseDependencies.empty())
					ImGui::TextDisabled("None");
				for (const resource::AssetID &Dependent : Entry->ReverseDependencies)
				{
					const asset::ContentEntry *Resolved = Session.GetAssetRegistry().GetSnapshot().Find(Dependent);
					ImGui::BulletText("%s", Resolved == nullptr ? Dependent.c_str() : Resolved->VirtualPath.c_str());
				}
			}
			ImGui::Separator();
			if (ImGui::Button("Close", ImVec2(90.0f, 0.0f)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
	ImGui::End();
	if (Open != Panel.Open)
		Session.GetWorkspace().SetOpen(Panel.ID, Open);
}

void RenderMaterialEditor(auto &State, EditorSession &Session, action::EditorActionContext &Context)
{
	workspace::EditorPanelState &Panel = Session.GetWorkspace().GetPanel(workspace::EditorPanelID::MaterialEditor);
	if (!Panel.Open || Panel.Minimized)
		return;
	bool Open = Panel.Open;
	if (!ImGui::Begin("Material Editor", Panel.Closable ? &Open : nullptr))
	{
		ImGui::End();
		if (Open != Panel.Open)
			Session.GetWorkspace().SetOpen(Panel.ID, Open);
		return;
	}
	if (RenderPanelMinimizeControl(Session, Panel))
	{
		ImGui::End();
		return;
	}
	if (!State.MaterialSession.has_value())
	{
		ImGui::TextDisabled("Double-click a .material or .materialinstance asset to edit it.");
		ImGui::End();
		if (Open != Panel.Open)
			Session.GetWorkspace().SetOpen(Panel.ID, Open);
		return;
	}

	material::MaterialDocument &Document = State.MaterialSession->Edit();
	const auto Save = [&]()
	{
		try
		{
			State.MaterialSession->Save();
			Session.GetAssetRegistry().RequestRefresh(Context.Scheduler, true);
			Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Information, "Material",
										"Saved " + Document.Path.filename().string());
		}
		catch (const std::exception &Exception)
		{
			Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Material", Exception.what());
		}
	};
	const auto Revert = [&]()
	{
		try
		{
			State.MaterialSession->Reload(true);
		}
		catch (const std::exception &Exception)
		{
			Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Material", Exception.what());
		}
	};

	if (!State.MaterialSession->IsDirty())
		ImGui::BeginDisabled();
	if (ImGui::Button("Save"))
		Save();
	ImGui::SameLine();
	if (ImGui::Button("Revert"))
		Revert();
	if (!State.MaterialSession->IsDirty())
		ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("%s%s", Document.Path.filename().string().c_str(), State.MaterialSession->IsDirty() ? " *" : "");
	State.MaterialVirtualPathScratch = Session.GetProject().MakeVirtualPath(Document.Path);
	const asset::ContentEntry *RegistryEntry = Session.GetAssetRegistry().GetSnapshot().FindByVirtualPath(State.MaterialVirtualPathScratch);
	if (Document.Type == material::MaterialDocumentType::Material && RegistryEntry != nullptr &&
		!RegistryEntry->ReverseDependencies.empty())
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "Shared by %zu dependent assets", RegistryEntry->ReverseDependencies.size());
	}
	ImGui::Separator();

	material::MaterialDocument BeforeEdit = Document;
	bool Changed = InputText("Name", Document.Name);
	bool PipelineChanged = false;
	static constexpr std::array ShadingNames{"Unlit", "Default Lit", "Subsurface", "Clear Coat", "Cloth", "Hair"};
	static constexpr std::array BlendNames{"Opaque", "Masked", "Translucent", "Additive"};
	const usize ShadingIndex = static_cast<usize>(Document.Pipeline.ShadingModel);
	if (ImGui::BeginCombo("Shading Model", ShadingNames.at(ShadingIndex)))
	{
		for (usize Index = 0; Index < ShadingNames.size(); ++Index)
		{
			const bool Selected = Index == ShadingIndex;
			if (ImGui::Selectable(ShadingNames[Index], Selected))
			{
				Document.Pipeline.ShadingModel = static_cast<resource::MaterialShadingModel>(Index);
				Changed = PipelineChanged = true;
			}
			if (Selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	const usize BlendIndex = static_cast<usize>(Document.Pipeline.BlendMode);
	if (ImGui::BeginCombo("Blend Mode", BlendNames.at(BlendIndex)))
	{
		for (usize Index = 0; Index < BlendNames.size(); ++Index)
		{
			const bool Selected = Index == BlendIndex;
			if (ImGui::Selectable(BlendNames[Index], Selected))
			{
				Document.Pipeline.BlendMode = static_cast<resource::MaterialBlendMode>(Index);
				Changed = PipelineChanged = true;
			}
			if (Selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	PipelineChanged |= ImGui::Checkbox("Two Sided", &Document.Pipeline.TwoSided);
	ImGui::SameLine();
	PipelineChanged |= ImGui::Checkbox("Cast Shadows", &Document.Pipeline.CastsShadows);
	ImGui::SameLine();
	PipelineChanged |= ImGui::Checkbox("Receive Shadows", &Document.Pipeline.ReceivesShadows);
	Changed |= PipelineChanged;
	if (PipelineChanged && Document.Type == material::MaterialDocumentType::MaterialInstance)
		Document.PipelineOverride = Document.Pipeline;

	if (Document.Type == material::MaterialDocumentType::MaterialInstance && Document.Parent.has_value())
	{
		ImGui::SeparatorText("Parent");
		Changed |= InputText("Parent Asset ID", Document.Parent->ID);
		const bool InstanceParent = Document.Parent->Type == resource::AssetType::MaterialInstance;
		bool ParentIsInstance = InstanceParent;
		if (ImGui::Checkbox("Parent is Material Instance", &ParentIsInstance))
		{
			Document.Parent->Type = ParentIsInstance ? resource::AssetType::MaterialInstance : resource::AssetType::Material;
			Changed = true;
		}
	}

	ImGui::SeparatorText("PBR Factors");
	const auto TrackFactor = [&](const bool FactorChanged, auto &Override, const auto &Value)
	{
		if (FactorChanged && Document.Type == material::MaterialDocumentType::MaterialInstance)
			Override = Value;
		Changed |= FactorChanged;
	};
	const bool BaseColorChanged = ImGui::ColorEdit4("Base Color", &Document.Factors.BaseColor.x, ImGuiColorEditFlags_Float);
	const bool FactorColorPickerOpen = IsColorEditPickerPopupOpen("Base Color");
	TrackFactor(BaseColorChanged, Document.FactorOverrides.BaseColor, Document.Factors.BaseColor);
	const bool EmissiveChanged =
		ImGui::ColorEdit3("Emissive", &Document.Factors.Emissive.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
	const bool FactorColorPickerOpenAfterEmissive = FactorColorPickerOpen || IsColorEditPickerPopupOpen("Emissive");
	TrackFactor(EmissiveChanged, Document.FactorOverrides.Emissive, Document.Factors.Emissive);
	TrackFactor(ImGui::SliderFloat("Metallic", &Document.Factors.Metallic, 0.0f, 1.0f), Document.FactorOverrides.Metallic,
				Document.Factors.Metallic);
	TrackFactor(ImGui::SliderFloat("Roughness", &Document.Factors.Roughness, 0.0f, 1.0f), Document.FactorOverrides.Roughness,
				Document.Factors.Roughness);
	TrackFactor(ImGui::SliderFloat("Specular", &Document.Factors.Specular, 0.0f, 1.0f), Document.FactorOverrides.Specular,
				Document.Factors.Specular);
	TrackFactor(ImGui::DragFloat("Normal Scale", &Document.Factors.NormalScale, 0.01f, 0.0f, 8.0f), Document.FactorOverrides.NormalScale,
				Document.Factors.NormalScale);
	TrackFactor(ImGui::DragFloat("Occlusion Strength", &Document.Factors.OcclusionStrength, 0.01f, 0.0f, 8.0f),
				Document.FactorOverrides.OcclusionStrength, Document.Factors.OcclusionStrength);
	TrackFactor(ImGui::SliderFloat("Alpha Cutoff", &Document.Factors.AlphaCutoff, 0.0f, 1.0f), Document.FactorOverrides.AlphaCutoff,
				Document.Factors.AlphaCutoff);
	TrackFactor(ImGui::SliderFloat("Clear Coat", &Document.Factors.ClearCoat, 0.0f, 1.0f), Document.FactorOverrides.ClearCoat,
				Document.Factors.ClearCoat);
	TrackFactor(ImGui::SliderFloat("Clear Coat Roughness", &Document.Factors.ClearCoatRoughness, 0.0f, 1.0f),
				Document.FactorOverrides.ClearCoatRoughness, Document.Factors.ClearCoatRoughness);
	TrackFactor(ImGui::SliderFloat("Transmission", &Document.Factors.Transmission, 0.0f, 1.0f), Document.FactorOverrides.Transmission,
				Document.Factors.Transmission);
	TrackFactor(ImGui::DragFloat("Index of Refraction", &Document.Factors.IndexOfRefraction, 0.01f, 1.0f, 4.0f),
				Document.FactorOverrides.IndexOfRefraction, Document.Factors.IndexOfRefraction);

	ImGui::SeparatorText("Texture Bindings");
	static constexpr std::array SemanticNames{"Base Color", "Normal",	  "Metallic / Roughness", "Occlusion",	 "Emissive",
											  "Specular",	"Clear Coat", "Clear Coat Normal",	  "Transmission"};
	std::optional<usize> RemovedTexture;
	if (ImGui::BeginTable("MaterialTextures", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
	{
		ImGui::TableSetupColumn("Semantic", ImGuiTableColumnFlags_WidthFixed, 150.0f);
		ImGui::TableSetupColumn("Asset ID", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("UV", ImGuiTableColumnFlags_WidthFixed, 48.0f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
		ImGui::TableHeadersRow();
		for (usize Index = 0; Index < Document.Textures.size(); ++Index)
		{
			material::MaterialTextureReference &Texture = Document.Textures[Index];
			ImGui::PushID(static_cast<int32>(Index));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			const usize SemanticIndex = static_cast<usize>(Texture.Semantic);
			if (ImGui::BeginCombo("##Semantic", SemanticNames.at(SemanticIndex)))
			{
				for (usize Option = 0; Option < SemanticNames.size(); ++Option)
				{
					if (ImGui::Selectable(SemanticNames[Option], Option == SemanticIndex))
					{
						Texture.Semantic = static_cast<resource::MaterialTextureSemantic>(Option);
						Changed = true;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1.0f);
			Changed |= InputText("##TextureID", Texture.ID);
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload *Payload = ImGui::AcceptDragDropPayload("ASSET_CONTENT_PATH"))
				{
					const string Source(static_cast<const char *>(Payload->Data),
										(static_cast<const char *>(Payload->Data) + Payload->DataSize) - 1);
					const std::filesystem::path SourcePath(Source);
					const auto Entry =
						std::ranges::find(Session.GetAssetRegistry().GetSnapshot().Entries, SourcePath, &asset::ContentEntry::RelativePath);
					if (Entry != Session.GetAssetRegistry().GetSnapshot().Entries.end() &&
						Entry->AssetType == resource::AssetType::Texture2D)
					{
						Texture.ID = Entry->ID;
						Changed = true;
					}
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::SetNextItemWidth(-1.0f);
			Changed |= ImGui::InputScalar("##UV", ImGuiDataType_U32, &Texture.TextureCoordinateChannel);
			ImGui::TableSetColumnIndex(3);
			if (ImGui::SmallButton("X"))
				RemovedTexture = Index;
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (RemovedTexture.has_value())
	{
		Document.Textures.erase(Document.Textures.begin() + static_cast<isize>(*RemovedTexture));
		Changed = true;
	}
	if (ImGui::Button("Add Texture Binding"))
	{
		for (usize Index = 0; Index < SemanticNames.size(); ++Index)
		{
			const auto Semantic = static_cast<resource::MaterialTextureSemantic>(Index);
			if (std::ranges::none_of(Document.Textures, [Semantic](const material::MaterialTextureReference &Texture)
									 { return Texture.Semantic == Semantic; }))
			{
				Document.Textures.push_back({.Semantic = Semantic});
				Changed = true;
				break;
			}
		}
	}
	if (Changed)
		State.MaterialSession->BeginEditGesture(std::move(BeforeEdit));
	if (State.MaterialSession->HasActiveEditGesture() && !ImGui::IsAnyItemActive() && !FactorColorPickerOpenAfterEmissive)
		State.MaterialSession->EndEditGesture();
	ImGui::End();
	if (Open != Panel.Open)
		Session.GetWorkspace().SetOpen(Panel.ID, Open);
}

void RenderDiagnosticsPanel(auto &State, EditorSession &Session, core::threading::TaskScheduler &Scheduler,
							core::diagnostics::DiagnosticSink &Diagnostics, const bool OutputPanel)
{
	const workspace::EditorPanelID ID = OutputPanel ? workspace::EditorPanelID::Output : workspace::EditorPanelID::Diagnostics;
	workspace::EditorPanelState &Panel = Session.GetWorkspace().GetPanel(ID);
	if (!Panel.Open || Panel.Minimized)
		return;
	bool Open = Panel.Open;
	if (ImGui::Begin(Panel.Name.c_str(), Panel.Closable ? &Open : nullptr))
	{
		if (RenderPanelMinimizeControl(Session, Panel))
		{
			ImGui::End();
			return;
		}
		if (OutputPanel)
		{
			const cook::CookPackageService &Cook = Session.GetCookPackageService();
			const cook::CookPackageState CookState = Cook.GetState();
			if (CookState != cook::CookPackageState::Idle)
			{
				constexpr std::array StateNames{"Idle", "Cooking", "Publishing", "Completed", "Cancelled", "Failed"};
				ImGui::Text("Cook/package: %s", StateNames.at(static_cast<usize>(CookState)));
				ImGui::ProgressBar(Cook.GetProgress(), ImVec2(-1.0f, 0.0f));
				if (!Cook.GetDiagnostic().empty())
					ImGui::TextWrapped("%s", Cook.GetDiagnostic().c_str());
				if (Cook.GetResult().has_value())
					ImGui::TextWrapped("Output: %s", Cook.GetResult()->PackageDirectory.string().c_str());
				ImGui::Separator();
			}
		}
		else
		{
			ImGui::Text("Task scheduler: %u workers", Scheduler.GetWorkerCount());
			ImGui::Text("Pending work: %zu / %zu", Scheduler.GetPendingTaskCount(), Scheduler.GetCapacity());
			ImGui::Separator();
		}
		std::vector<core::diagnostics::Diagnostic> &Entries = State.DiagnosticsScratch;
		Diagnostics.SnapshotInto(Entries);
		for (const core::diagnostics::Diagnostic &Entry : Entries)
		{
			const ImVec4 Color =
				Entry.Severity >= core::diagnostics::DiagnosticSeverity::Error
					? ImVec4(1.0f, 0.35f, 0.32f, 1.0f)
					: (Entry.Severity == core::diagnostics::DiagnosticSeverity::Warning ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f)
																						: ImGui::GetStyleColorVec4(ImGuiCol_Text));
			ImGui::TextColored(Color, "[%s] %s", Entry.Category.c_str(), Entry.Message.c_str());
		}
		if (Entries.empty())
			ImGui::TextDisabled(OutputPanel ? "No output has been produced." : "No diagnostics.");
	}
	ImGui::End();
	if (Open != Panel.Open)
		Session.GetWorkspace().SetOpen(Panel.ID, Open);
}

void RenderRecoveryModal(auto &State, EditorSession &Session, core::diagnostics::DiagnosticSink &Diagnostics)
{
	if (!State.RecoveryScanned)
	{
		State.RecoveryScanned = true;
		try
		{
			State.RecoveryCandidates = Session.ScanRecovery();
			if (!State.RecoveryCandidates.empty())
				ImGui::OpenPopup("Recover Unsaved Work");
		}
		catch (const std::exception &Exception)
		{
			Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Recovery", Exception.what());
		}
	}
	if (!ImGui::BeginPopupModal("Recover Unsaved Work", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	ImGui::TextUnformatted("The editor found recoverable scene snapshots.");
	ImGui::TextDisabled("Choose the newest snapshot to restore, or discard all recovery data.");
	ImGui::Separator();
	for (const recovery::EditorRecoveryCandidate &Candidate : State.RecoveryCandidates)
	{
		ImGui::Text("%s", Candidate.DocumentName.c_str());
		ImGui::SameLine();
		State.UITextScratch.clear();
		std::format_to(std::back_inserter(State.UITextScratch), "{}", Candidate.Revision);
		ImGui::TextDisabled("revision %s", State.UITextScratch.c_str());
		if (!Candidate.OriginalPath.empty())
			ImGui::TextDisabled("%s", Candidate.OriginalPath.string().c_str());
	}
	ImGui::Separator();
	if (ImGui::Button("Recover Newest", ImVec2(140.0f, 0.0f)))
	{
		try
		{
			Session.RecoverDocument(State.RecoveryCandidates.front());
			State.RecoveryCandidates.clear();
			ImGui::CloseCurrentPopup();
		}
		catch (const std::exception &Exception)
		{
			Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Recovery", Exception.what());
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Discard All", ImVec2(110.0f, 0.0f)))
	{
		try
		{
			for (const recovery::EditorRecoveryCandidate &Candidate : State.RecoveryCandidates)
				Session.DiscardRecovery(Candidate);
			State.RecoveryCandidates.clear();
			ImGui::CloseCurrentPopup();
		}
		catch (const std::exception &Exception)
		{
			Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Recovery", Exception.what());
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Later", ImVec2(80.0f, 0.0f)))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void RenderPreferences(auto &State, EditorSession &Session, core::diagnostics::DiagnosticSink &Diagnostics)
{
	if (!Session.IsPreferencesOpen())
	{
		State.PreferencesDraft.reset();
		return;
	}
	if (!State.PreferencesDraft.has_value())
		State.PreferencesDraft = Session.GetPreferences();
	bool Open = true;
	if (ImGui::Begin("Preferences", &Open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking))
	{
		preferences::EditorPreferences &Preferences = *State.PreferencesDraft;
		ImGui::SeparatorText("Autosave & Recovery");
		ImGui::Checkbox("Enable autosave", &Preferences.AutosaveEnabled);
		const uint32 MinimumInterval = 10;
		const uint32 MaximumInterval = 3'600;
		ImGui::SliderScalar("Autosave interval", ImGuiDataType_U32, &Preferences.AutosaveIntervalSeconds, &MinimumInterval,
							&MaximumInterval, "%u seconds");
		const uint32 MinimumQuietPeriod = 0;
		const uint32 MaximumQuietPeriod = 60;
		ImGui::SliderScalar("Quiet period", ImGuiDataType_U32, &Preferences.AutosaveQuietPeriodSeconds, &MinimumQuietPeriod,
							&MaximumQuietPeriod, "%u seconds");

		ImGui::SeparatorText("Editing");
		const uint32 MinimumHistory = 32;
		const uint32 MaximumHistory = 65'536;
		ImGui::SliderScalar("Command history", ImGuiDataType_U32, &Preferences.CommandHistoryCapacity, &MinimumHistory, &MaximumHistory,
							"%u commands", ImGuiSliderFlags_Logarithmic);
		ImGui::Checkbox("Show grid by default", &Preferences.ShowGridByDefault);
		ImGui::Checkbox("Enable transform snapping", &Preferences.TransformSnappingEnabled);
		ImGui::DragFloat("Translation increment", &Preferences.TranslationSnap, 0.05f, 0.001f, 100'000.0f, "%.3f");
		ImGui::DragFloat("Rotation increment", &Preferences.RotationSnapDegrees, 0.5f, 0.001f, 360.0f, "%.3f degrees");
		ImGui::DragFloat("Scale increment", &Preferences.ScaleSnap, 0.01f, 0.001f, 100.0f, "%.3f");

		ImGui::SeparatorText("Viewport Camera");
		ImGui::DragFloat("Movement speed", &Preferences.CameraMoveSpeed, 0.1f, 0.01f, 10'000.0f, "%.2f");
		ImGui::DragFloat("Look sensitivity", &Preferences.CameraLookSensitivity, 0.005f, 0.001f, 10.0f, "%.3f");

		ImGui::Separator();
		if (ImGui::Button("Apply", ImVec2(100.0f, 0.0f)))
		{
			try
			{
				Session.SetPreferences(Preferences);
				State.DefaultViewportSettings.Overlays.Grid = Preferences.ShowGridByDefault;
				for (auto &Viewport : State.Viewports)
					Viewport.Settings.Overlays.Grid = Preferences.ShowGridByDefault;
				Session.SetPreferencesOpen(false);
				State.PreferencesDraft.reset();
			}
			catch (const std::exception &Exception)
			{
				Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Preferences", Exception.what());
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
		{
			Session.SetPreferencesOpen(false);
			State.PreferencesDraft.reset();
		}
	}
	ImGui::End();
	if (!Open)
	{
		Session.SetPreferencesOpen(false);
		State.PreferencesDraft.reset();
	}
}

void InvokeAction(action::EditorActionRegistry &Actions, const action::EditorActionID ID, action::EditorActionContext &Context)
{
	(void)Actions.Invoke(ID, Context);
}

void RenderMenuBar(auto &State, EditorSession &Session, action::EditorActionRegistry &Actions, action::EditorActionContext &Context,
				   bool &CreateViewportRequested, bool &CloseProjectRequested)
{
	if (!ImGui::BeginMenuBar())
		return;
	constexpr std::array Menus{
		std::pair{action::EditorActionCategory::File, "File"},	   std::pair{action::EditorActionCategory::Edit, "Edit"},
		std::pair{action::EditorActionCategory::View, "View"},	   std::pair{action::EditorActionCategory::Assets, "Assets"},
		std::pair{action::EditorActionCategory::Build, "Build"},   std::pair{action::EditorActionCategory::Test, "Test"},
		std::pair{action::EditorActionCategory::Window, "Window"}, std::pair{action::EditorActionCategory::Help, "Help"}};
	std::vector<const action::EditorActionDescriptor *> &Snapshot = State.ActionDescriptorsScratch;
	Actions.SnapshotInto(Snapshot);
	for (const auto &[Category, Name] : Menus)
	{
		if (!ImGui::BeginMenu(Name))
			continue;
		for (const action::EditorActionDescriptor *Descriptor : Snapshot)
		{
			if (Descriptor->Category != Category)
				continue;
			const bool Enabled = Actions.CanExecute(Descriptor->ID, Context);
			const bool Checked = Actions.IsChecked(Descriptor->ID, Context);
			if (ImGui::MenuItem(Descriptor->DisplayName.c_str(), nullptr, Checked, Enabled))
				InvokeAction(Actions, Descriptor->ID, Context);
			if (!Enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				const string DisabledReason = Actions.GetDisabledReason(Descriptor->ID, Context);
				ImGui::SetTooltip("%s", DisabledReason.c_str());
			}
		}
		if (Category == action::EditorActionCategory::View)
		{
			for (const workspace::EditorPanelState &Panel : Session.GetWorkspace().GetPanels())
			{
				if (!Panel.Closable)
					continue;
				bool Open = Panel.Open;
				if (ImGui::MenuItem(Panel.Name.c_str(), nullptr, &Open))
					Session.GetWorkspace().SetOpen(Panel.ID, Open);
			}
		}
		if (Category == action::EditorActionCategory::Window)
		{
			if (ImGui::MenuItem("New Viewport"))
				CreateViewportRequested = true;
		}
		if (Category == action::EditorActionCategory::File)
		{
			ImGui::Separator();
			if (ImGui::MenuItem("Close Project"))
				CloseProjectRequested = true;
		}
		ImGui::EndMenu();
	}
	ImGui::EndMenuBar();
}

[[nodiscard]] string_view ActionCategoryName(const action::EditorActionCategory Category) noexcept
{
	switch (Category)
	{
	case action::EditorActionCategory::File:
		return "File";
	case action::EditorActionCategory::Edit:
		return "Edit";
	case action::EditorActionCategory::Transform:
		return "Transform";
	case action::EditorActionCategory::Assets:
		return "Assets";
	case action::EditorActionCategory::Build:
		return "Build";
	case action::EditorActionCategory::Test:
		return "Test";
	case action::EditorActionCategory::View:
		return "View";
	case action::EditorActionCategory::Window:
		return "Window";
	case action::EditorActionCategory::Help:
		return "Help";
	}
	return "Unknown";
}

void RenderHelpModals(auto &State, EditorSession &Session, action::EditorActionRegistry &Actions)
{
	if (Session.IsCommandReferenceOpen())
		ImGui::OpenPopup("Command Reference");
	bool CommandReferenceOpen = Session.IsCommandReferenceOpen();
	ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal("Command Reference", &CommandReferenceOpen, ImGuiWindowFlags_NoCollapse))
	{
		ImGui::TextUnformatted("Every menu, ribbon, and shortcut command is routed through the editor action registry.");
		ImGui::Separator();
		std::vector<const action::EditorActionDescriptor *> &Descriptors = State.ActionDescriptorsScratch;
		Actions.SnapshotInto(Descriptors);
		if (ImGui::BeginTable("CommandReferenceTable", 3,
							  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
							  ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing())))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 180.0f);
			ImGui::TableSetupColumn("Purpose", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			for (const action::EditorActionDescriptor *Descriptor : Descriptors)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(ActionCategoryName(Descriptor->Category).data());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(Descriptor->DisplayName.c_str());
				ImGui::TableSetColumnIndex(2);
				ImGui::TextWrapped("%s", Descriptor->Description.c_str());
			}
			ImGui::EndTable();
		}
		if (ImGui::Button("Close"))
		{
			CommandReferenceOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	Session.SetCommandReferenceOpen(CommandReferenceOpen);

	if (Session.IsAboutOpen())
		ImGui::OpenPopup("About Engine");
	bool AboutOpen = Session.IsAboutOpen();
	ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal("About Engine", &AboutOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
	{
		const runtime::project::ProjectDescriptor &Project = Session.GetProject().GetDescriptor();
		ImGui::TextUnformatted("OpenFrame");
		ImGui::Separator();
		ImGui::Text("Engine ABI: %u", core::GetEngineABIVersion());
		ImGui::Text("Project: %s", Project.Name.c_str());
		ImGui::Text("Project ID: %s", Project.ID.ToString().c_str());
		ImGui::Text("Renderer: OpenGL 4.6 core");
		ImGui::Text("Asset root: %s", Session.GetProject().GetPaths().Content.string().c_str());
		if (ImGui::Button("Close"))
		{
			AboutOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	Session.SetAboutOpen(AboutOpen);
}

void RenderToolbar(auto &State, EditorSession &Session, action::EditorActionRegistry &Actions, action::EditorActionContext &Context,
				   const EditorIconRegistry &Icons)
{
	ImGui::BeginChild("PrimaryToolbar", ImVec2(0.0f, 76.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	if (State.RenderedToolbarWorkspace != State.ActiveWorkspace)
	{
		ImGui::SetScrollX(0.0f);
		State.RenderedToolbarWorkspace = State.ActiveWorkspace;
	}
	for (const workspace::EditorToolbarGroup &Group : Session.GetWorkspace().GetToolbarGroups())
	{
		if (Group.Name == "Play" || !workspace::IsVisible(Group.VisibleIn, State.ActiveWorkspace))
			continue;
		ImGui::PushID(Group.Name.c_str());
		for (const action::EditorActionID ID : Group.Actions)
		{
			const action::EditorActionDescriptor *Descriptor = Actions.Find(ID);
			if (Descriptor == nullptr)
				continue;
			const bool Enabled = Actions.CanExecute(ID, Context);
			const bool Checked = Actions.IsChecked(ID, Context);
			const string_view Icon = Icons.Find(Descriptor->Icon);
			string &Label = State.ToolbarLabelScratch;
			if (Icon.empty())
				Label = Descriptor->DisplayName;
			else
			{
				Label.assign(Icon);
				Label.push_back('\n');
				Label.append(Descriptor->DisplayName);
			}
			ImGui::BeginDisabled(!Enabled);
			if (Checked)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			if (ImGui::Button(Label.c_str(), ImVec2(0.0f, 54.0f)))
			{
				if (Group.Name == "Primitives")
					State.DeferredToolbarActionsScratch.push_back(ID);
				else
					InvokeAction(Actions, ID, Context);
			}
			if (Checked)
				ImGui::PopStyleColor();
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				const string Tooltip = Enabled ? Descriptor->Description : Actions.GetDisabledReason(ID, Context);
				ImGui::SetTooltip("%s", Tooltip.c_str());
			}
			ImGui::SameLine();
		}
		ImGui::PopID();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();
	}
	ImGui::EndChild();
}

void RenderWorkspaceStrip(auto &State, action::EditorActionRegistry &Actions, action::EditorActionContext &Context,
						  const EditorIconRegistry &Icons)
{
	ImGui::BeginChild("WorkspaceStrip", ImVec2(0.0f, 40.0f), ImGuiChildFlags_Borders);
	constexpr std::array PlayActions{action::IDs::Play, action::IDs::Simulate, action::IDs::Pause, action::IDs::Step,
									 action::IDs::Standalone};
	for (const action::EditorActionID ID : PlayActions)
	{
		const action::EditorActionDescriptor *Descriptor = Actions.Find(ID);
		if (Descriptor == nullptr)
			continue;
		const bool Enabled = Actions.CanExecute(ID, Context);
		const string_view Icon = Icons.Find(Descriptor->Icon);
		ImGui::BeginDisabled(!Enabled);
		ImGui::PushID(static_cast<int32>(ID));
		if (ImGui::Button(Icon.empty() ? Descriptor->DisplayName.c_str() : Icon.data(), ImVec2(34.0f, 28.0f)))
			InvokeAction(Actions, ID, Context);
		ImGui::PopID();
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			const string Tooltip = Enabled ? Descriptor->Description : Actions.GetDisabledReason(ID, Context);
			ImGui::SetTooltip("%s", Tooltip.c_str());
		}
		ImGui::SameLine();
	}
	ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
	ImGui::SameLine();
	for (const workspace::EditorWorkspaceDescriptor &Workspace : workspace::EditorWorkspace::GetWorkspaceDescriptors())
	{
		if (Workspace.ID != workspace::EditorWorkspaceID::Home)
			ImGui::SameLine();
		const bool WasActive = State.ActiveWorkspace == Workspace.ID;
		if (WasActive)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
		ImGui::PushID(static_cast<int32>(Workspace.ID));
		if (ImGui::Button(Workspace.Name.data(), ImVec2(94.0f, 28.0f)))
			State.ActiveWorkspace = Workspace.ID;
		ImGui::PopID();
		if (WasActive)
			ImGui::PopStyleColor();
	}
	ImGui::EndChild();
}

void RenderMinimizedPanelBar(EditorSession &Session)
{
	bool HasMinimizedPanels = false;
	for (const workspace::EditorPanelState &Panel : Session.GetWorkspace().GetPanels())
		HasMinimizedPanels = HasMinimizedPanels || (Panel.Open && Panel.Minimized);
	if (!HasMinimizedPanels)
		return;

	ImGui::BeginChild("MinimizedPanels", ImVec2(0.0f, 31.0f), ImGuiChildFlags_Borders);
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Minimized");
	for (const workspace::EditorPanelState &Panel : Session.GetWorkspace().GetPanels())
	{
		if (!Panel.Open || !Panel.Minimized)
			continue;
		ImGui::SameLine();
		ImGui::PushID(static_cast<int32>(Panel.ID));
		if (ImGui::SmallButton(Panel.Name.c_str()))
			Session.GetWorkspace().SetMinimized(Panel.ID, false);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Restore %s", Panel.Name.c_str());
		ImGui::PopID();
	}
	ImGui::EndChild();
}

[[nodiscard]] EditorViewportRegion RenderViewport(auto &State, EditorSession &Session, action::EditorActionContext &Context,
												  action::EditorActionRegistry &Actions, const EditorViewportPresentation &Presentation,
												  const ImVec2 FramebufferScale, bool &CreateRequested, bool &CloseRequested,
												  bool &ToggleProjectionRequested)
{
	workspace::EditorPanelState &Panel = Session.GetWorkspace().GetPanel(workspace::EditorPanelID::Viewport);
	EditorViewportRegion Region{.View = Presentation.View};
	if (!Presentation.View.IsValid())
		throw std::invalid_argument("Editor UI cannot render a viewport with an invalid identity");
	if (!Presentation.Closable && (!Panel.Open || Panel.Minimized))
		return Region;
	auto ViewportState = std::ranges::find_if(State.Viewports, [&](const auto &Candidate) { return Candidate.View == Presentation.View; });
	if (ViewportState == State.Viewports.end())
	{
		State.Viewports.push_back({.View = Presentation.View, .Settings = State.DefaultViewportSettings});
		ViewportState = std::prev(State.Viewports.end());
	}
	Region.Settings = ViewportState->Settings;
	bool Open = true;
	string &WindowName = State.UITextScratch;
	WindowName = Presentation.Closable ? Presentation.Name : "Viewport";
	if (Presentation.Closable)
		std::format_to(std::back_inserter(WindowName), "###Viewport-{}", Presentation.View.Value);
	if (!ImGui::Begin(WindowName.c_str(), Presentation.Closable ? &Open : nullptr,
					  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::End();
		CloseRequested = Presentation.Closable && !Open;
		return Region;
	}
	if (!Presentation.Closable && RenderPanelMinimizeControl(Session, Panel))
	{
		ImGui::End();
		return Region;
	}
	ImVec2 ViewportFramebufferScale = FramebufferScale;
	if (core::Window *ManagedWindow = State.WindowBridge->GetManagedWindow(*ImGui::GetWindowViewport()))
	{
		Region.Window = ManagedWindow->GetID();
		const core::WindowExtent WindowExtent = ManagedWindow->GetExtent();
		const core::WindowExtent FramebufferExtent = ManagedWindow->GetFramebufferExtent();
		if (WindowExtent.IsValid())
		{
			ViewportFramebufferScale = ImVec2(static_cast<float32>(FramebufferExtent.Width) / static_cast<float32>(WindowExtent.Width),
											  static_cast<float32>(FramebufferExtent.Height) / static_cast<float32>(WindowExtent.Height));
		}
	}
	const ImVec2 Minimum = ImGui::GetCursorScreenPos();
	const ImVec2 Available = ImGui::GetContentRegionAvail();
	Region.Left = Minimum.x;
	Region.Top = Minimum.y;
	Region.Width = std::max(Available.x, 1.0f);
	Region.Height = std::max(Available.y, 1.0f);
	Region.PixelExtent = {.Width = static_cast<uint32>(std::max(1.0f, std::floor(Region.Width * ViewportFramebufferScale.x))),
						  .Height = static_cast<uint32>(std::max(1.0f, std::floor(Region.Height * ViewportFramebufferScale.y)))};
	if (Presentation.Output.Color.IsValid())
	{
		ImGui::Image(ImTextureRef(static_cast<ImTextureID>(Presentation.Output.Color.Texture)), Available, ImVec2(0.0f, 1.0f),
					 ImVec2(1.0f, 0.0f));
	}
	else
	{
		const ImU32 Background = ImGui::GetColorU32(ImVec4(0.018f, 0.023f, 0.032f, 1.0f));
		ImGui::GetWindowDrawList()->AddRectFilled(Minimum, ImVec2(Minimum.x + Available.x, Minimum.y + Available.y), Background);
		ImGui::SetCursorScreenPos(ImVec2(Minimum.x + 18.0f, Minimum.y + 18.0f));
		ImGui::TextDisabled("Preparing viewport...");
		ImGui::Dummy(Available);
	}
	Region.Hovered = ImGui::IsItemHovered();
	Region.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (Region.Focused)
	{
		State.ActiveViewport = Presentation.View;
		Context.ActiveViewportSettings = &ViewportState->Settings;
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload *Payload = ImGui::AcceptDragDropPayload("ASSET_CONTENT_PATH"))
		{
			const string Path(static_cast<const char *>(Payload->Data), (static_cast<const char *>(Payload->Data) + Payload->DataSize) - 1);
			try
			{
				CreateMeshObjectFromContent(Session, Context, Path);
			}
			catch (const std::exception &Exception)
			{
				Context.Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "Viewport", Exception.what());
			}
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::SetCursorScreenPos(ImVec2(Minimum.x + 10.0f, Minimum.y + 10.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.061f, 0.075f, 0.92f));
	ImGui::BeginChild("ViewportControls", ImVec2(300.0f, 34.0f), ImGuiChildFlags_Borders,
					  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	if (ImGui::Button(Presentation.Projection == CameraProjectionMode::Perspective ? "Perspective" : "Orthographic"))
		ToggleProjectionRequested = true;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Switch this viewport between perspective and orthographic projection");
	ImGui::SameLine();
	static constexpr std::array ViewModeNames{"Lit", "Unlit", "Wireframe", "Normals", "Depth", "Object ID", "Overdraw"};
	static_assert(ViewModeNames.size() == static_cast<usize>(pipeline::render::ViewportViewMode::Count));
	const usize ViewModeIndex = static_cast<usize>(ViewportState->Settings.ViewMode);
	if (ImGui::Button(ViewModeNames.at(ViewModeIndex)))
		ImGui::OpenPopup("ViewportViewMode");
	if (ImGui::BeginPopup("ViewportViewMode"))
	{
		static constexpr std::array ViewModeActions{action::IDs::ViewLit,	  action::IDs::ViewUnlit, action::IDs::ViewWireframe,
													action::IDs::ViewNormals, action::IDs::ViewDepth, action::IDs::ViewObjectID,
													action::IDs::ViewOverdraw};
		for (usize Index = 0; Index < ViewModeNames.size(); ++Index)
		{
			if (ImGui::MenuItem(ViewModeNames[Index], nullptr, Index == ViewModeIndex))
				InvokeAction(Actions, ViewModeActions[Index], Context);
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("+ View"))
		CreateRequested = true;
	ImGui::SameLine();
	if (ImGui::Button("Show"))
		ImGui::OpenPopup("ViewportOverlays");
	if (ImGui::BeginPopup("ViewportOverlays"))
	{
		const auto OverlayAction = [&](const char *Name, const action::EditorActionID ID)
		{
			const bool Enabled = Actions.CanExecute(ID, Context);
			if (ImGui::MenuItem(Name, nullptr, Actions.IsChecked(ID, Context), Enabled))
				InvokeAction(Actions, ID, Context);
			if (!Enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				const string DisabledReason = Actions.GetDisabledReason(ID, Context);
				ImGui::SetTooltip("%s", DisabledReason.c_str());
			}
		};
		OverlayAction("Grid", action::IDs::OverlayGrid);
		OverlayAction("Bounds", action::IDs::OverlayBounds);
		OverlayAction("Skeletons", action::IDs::OverlaySkeletons);
		OverlayAction("Cameras", action::IDs::OverlayCameras);
		OverlayAction("Lights", action::IDs::OverlayLights);
		OverlayAction("Culling", action::IDs::OverlayCulling);
		OverlayAction("Selection", action::IDs::OverlaySelection);
		ImGui::Separator();
		OverlayAction("Render Statistics", action::IDs::OverlayRenderStatistics);
		OverlayAction("Render Graph", action::IDs::OverlayRenderGraph);
		ImGui::EndPopup();
	}
	Region.Settings = ViewportState->Settings;
	ImGui::EndChild();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	if (ViewportState->Settings.Overlays.RenderStatistics && Presentation.Output.IsValid())
	{
		const auto &Statistics = Presentation.Output.RenderStatistics;
		ImGui::SetCursorScreenPos(ImVec2(Minimum.x + Available.x - 220.0f, Minimum.y + 10.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.04f, 0.05f, 0.9f));
		ImGui::BeginChild("ViewportStatistics", ImVec2(210.0f, 112.0f), ImGuiChildFlags_Borders,
						  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::TextUnformatted("Render Statistics");
		ImGui::Separator();
		ImGui::Text("Objects: %u", Statistics.SubmittedObjects);
		ImGui::Text("Candidates: %u", Statistics.CandidateInstances);
		ImGui::Text("Batches / draws: %u / %u", Statistics.RenderBatches, Statistics.DrawCalls);
		ImGui::Text("Debug lines: %u", Statistics.DebugLines);
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}
	if (ViewportState->Settings.Overlays.RenderGraph && Presentation.Output.GraphInspection != nullptr)
	{
		const pipeline::graph::RenderGraphInspection &Inspection = *Presentation.Output.GraphInspection;
		const float32 Height = std::min(Available.y * 0.55f, 390.0f);
		ImGui::SetCursorScreenPos(ImVec2(Minimum.x + Available.x - 290.0f, Minimum.y + Available.y - Height - 10.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.04f, 0.05f, 0.94f));
		ImGui::BeginChild("ViewportRenderGraph", ImVec2(280.0f, Height), ImGuiChildFlags_Borders);
		State.UITextScratch = "Render Graph - Frame ";
		std::format_to(std::back_inserter(State.UITextScratch), "{}", Inspection.FrameSerial);
		ImGui::TextUnformatted(State.UITextScratch.c_str());
		ImGui::TextDisabled("%zu passes, %zu textures, %zu buffers", Inspection.Passes.size(), Inspection.Textures.size(),
							Inspection.Buffers.size());
		ImGui::Separator();
		for (usize Index = 0; Index < Inspection.Passes.size(); ++Index)
			ImGui::BulletText("%02zu  %s", Index, Inspection.Passes[Index].c_str());
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}
	ImGui::End();
	CloseRequested = Presentation.Closable && !Open;
	(void)Panel;
	return Region;
}
} // namespace

bool EditorViewportRegion::IsValid() const noexcept
{
	return this->PixelExtent.IsValid() && this->Width > 0.0f && this->Height > 0.0f;
}

bool EditorViewportRegion::Contains(const float64 X, const float64 Y) const noexcept
{
	return X >= this->Left && Y >= this->Top && X < this->Left + this->Width && Y < this->Top + this->Height;
}

EditorUserInterface::EditorUserInterface(core::Window &Window) : StateData(std::make_unique<State>())
{
	this->StateData->Window = &Window;
	this->StateData->PropertyValueScratch.reserve(512);
	this->StateData->PropertySearchScratch.reserve(256);
	this->StateData->PropertyFilterScratch.reserve(128);
	this->StateData->PropertyListScratch.reserve(16);
	this->StateData->MaterialNameScratch.reserve(128);
	this->StateData->HierarchyFilterScratch.reserve(128);
	this->StateData->ToolbarLabelScratch.reserve(128);
	this->StateData->MaterialVirtualPathScratch.reserve(256);
	this->StateData->ThumbnailDiagnosticScratch.reserve(256);
	this->StateData->UITextScratch.reserve(256);
	this->StateData->Context = std::make_unique<EditorUIContext>();
	this->StateData->Renderer = std::make_unique<EditorUIRenderer>(*this->StateData->Context);
}

EditorUserInterface::~EditorUserInterface()
{
	if (this->StateData == nullptr)
		return;
	if (this->StateData->Renderer != nullptr && this->StateData->Renderer->IsInitialized())
		std::terminate();
	if (this->StateData->LayoutStore != nullptr && this->StateData->LayoutStore->IsDirty())
	{
		try
		{
			this->StateData->LayoutStore->Flush();
		}
		catch (...)
		{
		}
	}
}

void EditorUserInterface::InitializeRenderer()
{
	this->StateData->Renderer->Initialize();
}

void EditorUserInterface::ShutdownRenderer()
{
	if (this->StateData == nullptr || this->StateData->Renderer == nullptr)
		return;
	std::scoped_lock ContextLock(this->StateData->Context->GetMutex());
	this->StateData->Context->Activate();
	if (this->StateData->WindowBridge != nullptr)
		this->StateData->WindowBridge->PrepareDetachedWindowTransfers();
	ReleaseThumbnailTextures(*this->StateData);
	this->StateData->Renderer->Shutdown();
}

void EditorUserInterface::AttachWindowManager(core::WindowManager &Manager)
{
	std::scoped_lock ContextLock(this->StateData->Context->GetMutex());
	this->StateData->Context->Activate();
	if (this->StateData->WindowBridge != nullptr)
		return;
	auto Bridge = std::make_unique<EditorUIWindowBridge>(Manager, *this->StateData->Window);
	Bridge->Install();
	this->StateData->WindowBridge = std::move(Bridge);
}

void EditorUserInterface::QueueInputEvent(const core::input::InputEvent &Event)
{
	if (Event.Type != core::input::InputEventType::Key && Event.Type != core::input::InputEventType::Text)
		return;
	std::scoped_lock InputLock(this->StateData->BufferedInputMutex);
	this->StateData->BufferedKeyboardEvents.push_back(Event);
}

EditorUIFrame EditorUserInterface::BuildFrame(const core::ApplicationFrame &Frame, EditorSession &Session,
											  action::EditorActionRegistry &Actions, action::EditorActionContext &ActionContext,
											  core::diagnostics::DiagnosticSink &Diagnostics,
											  const std::span<const EditorViewportPresentation> Viewports, const bool AllowWindowMutation)
{
	std::scoped_lock ContextLock(this->StateData->Context->GetMutex());
	this->StateData->Context->Activate();
	EditorUIFrame Result = std::move(this->StateData->FrameScratch);
	Result.Viewports.clear();
	Result.ViewportTextures.clear();
	Result.CloseViewportRequests.clear();
	Result.ToggleViewportProjectionRequests.clear();
	Result.Windows.clear();
	Result.CreateViewportRequestCount = 0;
	Result.WantsKeyboard = false;
	Result.WantsPointer = false;
	CollectRetiredThumbnailTextures(*this->StateData);
	if (this->StateData->WindowBridge == nullptr)
		throw std::logic_error("Editor UI requires its managed window bridge before building frames");
	this->StateData->WindowBridge->ProcessEvents(Frame.WindowEvents);
	if (std::optional<string> CallbackDiagnostic = this->StateData->WindowBridge->TakeCallbackDiagnostic(); CallbackDiagnostic.has_value())
	{
		Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "EditorWindow", std::move(*CallbackDiagnostic));
	}
	if (!this->StateData->PreferencesInitialized)
	{
		this->StateData->PreferencesInitialized = true;
		this->StateData->DefaultViewportSettings.Overlays.Grid = Session.GetPreferences().ShowGridByDefault;
	}
	if (this->StateData->ContentBrowserStore == nullptr)
	{
		this->StateData->ContentBrowserStore =
			std::make_unique<preferences::EditorContentBrowserStore>(Session.GetProject().GetPaths().Layouts / "ContentBrowser.json");
		try
		{
			this->StateData->FavoriteAssetIDs = this->StateData->ContentBrowserStore->Load();
		}
		catch (const std::exception &Exception)
		{
			Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Warning, "ContentBrowser", Exception.what());
		}
	}
	FeedInput(*this->StateData, Frame, *this->StateData->Window, *this->StateData->WindowBridge);
	ImGui::NewFrame();
	if (this->StateData->LayoutStore == nullptr)
	{
		this->StateData->LayoutStore = std::make_unique<EditorLayoutStore>(Session.GetProject().GetPaths().Layouts / "EditorLayout.json");
		string DockingState;
		try
		{
			const ImVec2 DisplaySize = ImGui::GetMainViewport()->Size;
			const uint32 LayoutWidth = static_cast<uint32>(std::max(DisplaySize.x, 1.0f));
			const uint32 LayoutHeight = static_cast<uint32>(std::max(DisplaySize.y, 1.0f));
			std::vector<EditorViewportLayoutState> ViewportLayouts;
			if (this->StateData->LayoutStore->Load(Session.GetWorkspace(), DockingState, LayoutWidth, LayoutHeight, &ViewportLayouts))
			{
				if (!DockingState.empty())
					ImGui::LoadIniSettingsFromMemory(DockingState.data(), DockingState.size());
				this->StateData->Viewports.clear();
				std::ranges::sort(ViewportLayouts, {}, &EditorViewportLayoutState::View);
				const uint64 FirstView = Viewports.empty() ? 1U : Viewports.front().View.Value;
				if (!ViewportLayouts.empty() && ViewportLayouts.size() - 1U > std::numeric_limits<uint64>::max() - FirstView)
					throw std::overflow_error("restored editor viewport identity range is exhausted");
				for (usize Index = 0; Index < ViewportLayouts.size(); ++Index)
					this->StateData->Viewports.push_back(
						{.View = pipeline::render::RenderViewID{.Value = FirstView + Index}, .Settings = ViewportLayouts[Index].Settings});
				if (ViewportLayouts.size() > 1U)
					this->StateData->RestoredViewportCreations = static_cast<uint32>(ViewportLayouts.size() - 1U);
				this->StateData->LayoutInitialized = !DockingState.empty();
			}
		}
		catch (const std::exception &Exception)
		{
			Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Warning, "EditorLayout", Exception.what());
			this->StateData->LayoutFailureReported = true;
		}
		this->StateData->LayoutPrimed = true;
		this->StateData->LayoutResetGeneration = Session.GetWorkspace().GetLayoutResetGeneration();
	}
	bool CreateViewportRequested = false;
	RenderRecoveryModal(*this->StateData, Session, Diagnostics);
	RenderPreferences(*this->StateData, Session, Diagnostics);

	const ImGuiViewport *MainViewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(MainViewport->Pos);
	ImGui::SetNextWindowSize(MainViewport->Size);
	ImGui::SetNextWindowViewport(MainViewport->ID);
	const ImGuiWindowFlags HostFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
									   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
									   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("EditorHost", nullptr, HostFlags);
	ImGui::PopStyleVar(3);
	for (const EditorViewportPresentation &Viewport : Viewports)
	{
		if (std::ranges::find(this->StateData->Viewports, Viewport.View, &State::ViewportState::View) == this->StateData->Viewports.end())
		{
			this->StateData->Viewports.push_back({.View = Viewport.View, .Settings = this->StateData->DefaultViewportSettings});
		}
	}
	if (!this->StateData->ActiveViewport.IsValid() && !Viewports.empty())
		this->StateData->ActiveViewport = Viewports.front().View;
	const auto ActiveViewportState =
		std::ranges::find(this->StateData->Viewports, this->StateData->ActiveViewport, &State::ViewportState::View);
	ActionContext.ActiveViewportSettings =
		ActiveViewportState == this->StateData->Viewports.end() ? nullptr : &ActiveViewportState->Settings;
	RenderMenuBar(*this->StateData, Session, Actions, ActionContext, CreateViewportRequested, Result.CloseProjectRequest);
	RenderHelpModals(*this->StateData, Session, Actions);
	if (this->StateData->LayoutResetGeneration != Session.GetWorkspace().GetLayoutResetGeneration())
	{
		this->StateData->LayoutResetGeneration = Session.GetWorkspace().GetLayoutResetGeneration();
		this->StateData->LayoutInitialized = false;
	}
	RenderWorkspaceStrip(*this->StateData, Actions, ActionContext, this->StateData->Icons);
	RenderToolbar(*this->StateData, Session, Actions, ActionContext, this->StateData->Icons);
	RenderMinimizedPanelBar(Session);
	const ImGuiID DockspaceID = ImGui::GetID("EditorDockspace");
	const ImVec2 DockspaceSize = ImGui::GetContentRegionAvail();
	if (this->StateData->LayoutPrimed && (!this->StateData->LayoutInitialized || ImGui::DockBuilderGetNode(DockspaceID) == nullptr))
	{
		EditorDockspace::BuildReferenceLayout(DockspaceID, DockspaceSize.x, DockspaceSize.y);
		this->StateData->LayoutInitialized = true;
	}
	else if (ImGui::DockBuilderGetNode(DockspaceID) != nullptr)
	{
		EditorDockspace::ResizeReferenceLayoutIfUnmodified(DockspaceID, DockspaceSize.x, DockspaceSize.y);
	}
	ImGui::DockSpace(DockspaceID, DockspaceSize, ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::End();

	RenderPropertiesPanel(*this->StateData, Session, ActionContext);
	RenderExplorerPanel(*this->StateData, Session, Actions, ActionContext);
	RenderAssetBrowser(*this->StateData, Session, Actions, ActionContext);
	RenderMaterialEditor(*this->StateData, Session, ActionContext);
	RenderDiagnosticsPanel(*this->StateData, Session, ActionContext.Scheduler, Diagnostics, true);
	RenderDiagnosticsPanel(*this->StateData, Session, ActionContext.Scheduler, Diagnostics, false);
	std::vector<EditorViewportRegion> &ViewportRegions = Result.Viewports;
	std::vector<EditorViewportTextureBinding> &ViewportTextures = Result.ViewportTextures;
	std::vector<pipeline::render::RenderViewID> &CloseViewportRequests = Result.CloseViewportRequests;
	std::vector<pipeline::render::RenderViewID> &ToggleProjectionRequests = Result.ToggleViewportProjectionRequests;
	ViewportRegions.reserve(Viewports.size());
	ViewportTextures.reserve(Viewports.size());
	ToggleProjectionRequests.reserve(Viewports.size());
	for (const EditorViewportPresentation &Viewport : Viewports)
	{
		bool CloseRequested = false;
		bool ToggleProjectionRequested = false;
		ViewportRegions.push_back(RenderViewport(*this->StateData, Session, ActionContext, Actions, Viewport,
												 ImGui::GetIO().DisplayFramebufferScale, CreateViewportRequested, CloseRequested,
												 ToggleProjectionRequested));
		ViewportTextures.push_back({.View = Viewport.View, .Texture = Viewport.Output.Color});
		if (CloseRequested)
			CloseViewportRequests.push_back(Viewport.View);
		if (ToggleProjectionRequested)
			ToggleProjectionRequests.push_back(Viewport.View);
	}
	if (this->StateData->RestoredViewportCreations == 0)
	{
		std::erase_if(this->StateData->Viewports, [&Viewports](const State::ViewportState &State)
					  { return std::ranges::find(Viewports, State.View, &EditorViewportPresentation::View) == Viewports.end(); });
	}
	this->StateData->LayoutPrimed = true;

	ImGui::Render();
	for (const action::EditorActionID ID : this->StateData->DeferredToolbarActionsScratch)
		InvokeAction(Actions, ID, ActionContext);
	this->StateData->DeferredToolbarActionsScratch.clear();
	if (AllowWindowMutation)
		this->StateData->WindowBridge->UpdateWindows();
	ImGuiIO &IO = ImGui::GetIO();
	usize DockingStateSize = 0;
	const char *DockingState = ImGui::SaveIniSettingsToMemory(&DockingStateSize);
	const ImVec2 DisplaySize = ImGui::GetMainViewport()->Size;
	const uint32 LayoutWidth = static_cast<uint32>(std::max(DisplaySize.x, 1.0f));
	const uint32 LayoutHeight = static_cast<uint32>(std::max(DisplaySize.y, 1.0f));
	std::vector<EditorViewportLayoutState> &ViewportLayouts = this->StateData->ViewportLayoutsScratch;
	ViewportLayouts.clear();
	ViewportLayouts.reserve(this->StateData->Viewports.size());
	for (const State::ViewportState &Viewport : this->StateData->Viewports)
		ViewportLayouts.push_back({.View = Viewport.View.Value, .Settings = Viewport.Settings});
	this->StateData->LayoutStore->Capture(Session.GetWorkspace().GetPanels(), string_view(DockingState, DockingStateSize), LayoutWidth,
										  LayoutHeight, ViewportLayouts);
	if (this->StateData->LayoutStore->IsDirty())
	{
		try
		{
			this->StateData->LayoutStore->Flush();
			IO.WantSaveIniSettings = false;
			this->StateData->LayoutFailureReported = false;
		}
		catch (const std::exception &Exception)
		{
			if (!this->StateData->LayoutFailureReported)
			{
				Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Warning, "EditorLayout", Exception.what());
				this->StateData->LayoutFailureReported = true;
			}
		}
	}
	ActionContext.ActiveViewportSettings = nullptr;
	std::vector<EditorUIWindowFrame> &WindowFrames = Result.Windows;
	const ImGuiViewport *DrawMainViewport = ImGui::GetMainViewport();
	for (ImGuiViewport *Viewport : ImGui::GetPlatformIO().Viewports)
	{
		core::Window *ManagedWindow = this->StateData->WindowBridge->GetManagedWindow(*Viewport);
		if (ManagedWindow == nullptr || Viewport->DrawData == nullptr || !Viewport->DrawData->Valid || ManagedWindow->IsMinimized())
			continue;
		const usize DrawDataIndex = WindowFrames.size();
		if (this->StateData->DrawDataScratch.size() <= DrawDataIndex)
			this->StateData->DrawDataScratch.resize(DrawDataIndex + 1U);
		if (this->StateData->DrawDataScratch[DrawDataIndex] == nullptr)
			this->StateData->DrawDataScratch[DrawDataIndex] = std::make_shared<detail::EditorUIDrawData>();
		CloneDrawDataInto(*Viewport->DrawData, *this->StateData->DrawDataScratch[DrawDataIndex]);
		WindowFrames.push_back({.Window = ManagedWindow->GetID(),
								.ManagedWindow = ManagedWindow,
								.Main = Viewport == DrawMainViewport,
								.DrawData = this->StateData->DrawDataScratch[DrawDataIndex]});
	}
	const uint32 CreateViewportRequestCount =
		(CreateViewportRequested ? 1U : 0U) + std::exchange(this->StateData->RestoredViewportCreations, 0U);
	Result.CreateViewportRequestCount = CreateViewportRequestCount;
	Result.WantsKeyboard = IO.WantCaptureKeyboard;
	Result.WantsPointer = IO.WantCaptureMouse;
	return Result;
}

EditorUIFrame EditorUserInterface::BuildHomeFrame(const core::ApplicationFrame &Frame, project::ProjectHub &Projects,
												  core::diagnostics::DiagnosticSink &Diagnostics, const string_view ProjectDiagnostic,
												  const bool AllowWindowMutation)
{
	std::scoped_lock ContextLock(this->StateData->Context->GetMutex());
	this->StateData->Context->Activate();
	EditorUIFrame Result = std::move(this->StateData->FrameScratch);
	Result.Viewports.clear();
	Result.ViewportTextures.clear();
	Result.CloseViewportRequests.clear();
	Result.ToggleViewportProjectionRequests.clear();
	Result.Windows.clear();
	Result.OpenProjectRequest.reset();
	Result.CreateProjectRequest.reset();
	Result.CloseProjectRequest = false;
	Result.CreateViewportRequestCount = 0;
	Result.WantsKeyboard = false;
	Result.WantsPointer = false;
	if (this->StateData->WindowBridge == nullptr)
		throw std::logic_error("Editor Home requires its managed window bridge before building frames");
	this->StateData->WindowBridge->ProcessEvents(Frame.WindowEvents);
	if (std::optional<string> CallbackDiagnostic = this->StateData->WindowBridge->TakeCallbackDiagnostic(); CallbackDiagnostic.has_value())
		Diagnostics.Publish(core::diagnostics::DiagnosticSeverity::Error, "EditorWindow", std::move(*CallbackDiagnostic));
	FeedInput(*this->StateData, Frame, *this->StateData->Window, *this->StateData->WindowBridge);
	ImGui::NewFrame();

	const ImGuiViewport *MainViewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(MainViewport->Pos);
	ImGui::SetNextWindowSize(MainViewport->Size);
	ImGui::SetNextWindowViewport(MainViewport->ID);
	constexpr ImGuiWindowFlags HostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
										   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
										   ImGuiWindowFlags_NoNavFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("OpenFrameHome", nullptr, HostFlags);
	ImGui::PopStyleVar(3);
	const ImVec2 Available = ImGui::GetContentRegionAvail();
	const float32 ContentWidth = std::min(1'060.0f, Available.x - 48.0f);
	ImGui::SetCursorPos(ImVec2(std::max(24.0f, (Available.x - ContentWidth) * 0.5f), 56.0f));
	ImGui::BeginChild("HomeContent", ImVec2(ContentWidth, Available.y - 80.0f), ImGuiChildFlags_None);
	ImGui::PushFont(ImGui::GetIO().FontDefault, 28.0f);
	ImGui::TextUnformatted("OpenFrame");
	ImGui::PopFont();
	ImGui::TextDisabled("Create, open, and continue your worlds.");
	if (!ProjectDiagnostic.empty())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.38f, 0.34f, 1.0f));
		ImGui::TextWrapped("%.*s", static_cast<int32>(ProjectDiagnostic.size()), ProjectDiagnostic.data());
		ImGui::PopStyleColor();
	}
	ImGui::Spacing();
	ImGui::Spacing();
	if (ImGui::Button("New Project", ImVec2(170.0f, 42.0f)))
	{
		this->StateData->NewProjectName = "New Project";
		this->StateData->NewProjectParent = Projects.GetProjectsRoot();
		this->StateData->NewProjectParentText = this->StateData->NewProjectParent.string();
		this->StateData->NewProjectDialogOpen = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Open Project", ImVec2(170.0f, 42.0f)))
	{
		const core::DialogResult<core::FileDialogSelection> Selection =
			this->StateData->Window->ShowFileDialog({.Operation = core::FileDialogOperation::OpenFile,
													 .Title = "Open OpenFrame Project",
													 .InitialDirectory = Projects.GetProjectsRoot(),
													 .DefaultExtension = "engineproject",
													 .Filters = {{"OpenFrame Project", {"*.engineproject"}}},
													 .AddToRecent = false,
													 .RequireExistingPath = true});
		if (Selection.Accepted() && !Selection.Value->Paths.empty())
			Result.OpenProjectRequest = Selection.Value->Paths.front();
	}
	ImGui::Spacing();
	ImGui::SeparatorText("Recent Projects");
	Projects.Refresh();
	const std::span<const project::RecentProject> Recent = Projects.GetRecentProjects();
	if (Recent.empty())
	{
		ImGui::TextDisabled("No recent projects yet. Create a Baseplate project to get started.");
	}
	else
	{
		std::optional<std::filesystem::path> RemoveRecent;
		for (const project::RecentProject &Entry : Recent)
		{
			ImGui::PushID(Entry.DescriptorPath.string().c_str());
			ImGui::BeginDisabled(!Entry.Available);
			if (ImGui::Selectable(Entry.Name.c_str(), false, ImGuiSelectableFlags_None, ImVec2(0.0f, 48.0f)))
				Result.OpenProjectRequest = Entry.DescriptorPath;
			ImGui::EndDisabled();
			ImGui::SameLine(ContentWidth - 150.0f);
			if (ImGui::SmallButton("Remove"))
				RemoveRecent = Entry.DescriptorPath;
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", Entry.DescriptorPath.string().c_str());
			ImGui::PopID();
		}
		if (RemoveRecent.has_value())
			Projects.RemoveRecent(*RemoveRecent);
	}
	ImGui::EndChild();
	ImGui::End();

	if (this->StateData->NewProjectDialogOpen)
		ImGui::OpenPopup("Create Project");
	bool DialogOpen = this->StateData->NewProjectDialogOpen;
	ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
	if (ImGui::BeginPopupModal("Create Project", &DialogOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
	{
		ImGui::TextUnformatted("Baseplate");
		ImGui::TextDisabled("A lit world with a plane, camera, sun, shadows, and grid.");
		ImGui::Spacing();
		ImGui::InputText("Project name", &this->StateData->NewProjectName);
		if (ImGui::InputText("Location", &this->StateData->NewProjectParentText))
			this->StateData->NewProjectParent = std::filesystem::path(this->StateData->NewProjectParentText);
		if (ImGui::Button("Browse..."))
		{
			const core::DialogResult<core::FileDialogSelection> Selection =
				this->StateData->Window->ShowFileDialog({.Operation = core::FileDialogOperation::SelectFolder,
														 .Title = "Choose Project Location",
														 .InitialDirectory = this->StateData->NewProjectParent,
														 .RequireExistingPath = true});
			if (Selection.Accepted() && !Selection.Value->Paths.empty())
			{
				this->StateData->NewProjectParent = Selection.Value->Paths.front();
				this->StateData->NewProjectParentText = this->StateData->NewProjectParent.string();
			}
		}
		ImGui::Separator();
		if (ImGui::Button("Create", ImVec2(120.0f, 0.0f)))
		{
			Result.CreateProjectRequest = std::pair{this->StateData->NewProjectName, this->StateData->NewProjectParent};
			DialogOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
		{
			DialogOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	this->StateData->NewProjectDialogOpen = DialogOpen;

	ImGui::Render();
	if (AllowWindowMutation)
		this->StateData->WindowBridge->UpdateWindows();
	ImGuiIO &IO = ImGui::GetIO();
	Result.WantsKeyboard = IO.WantCaptureKeyboard;
	Result.WantsPointer = IO.WantCaptureMouse;
	const ImGuiViewport *DrawMainViewport = ImGui::GetMainViewport();
	for (ImGuiViewport *Viewport : ImGui::GetPlatformIO().Viewports)
	{
		core::Window *ManagedWindow = this->StateData->WindowBridge->GetManagedWindow(*Viewport);
		if (ManagedWindow == nullptr || Viewport->DrawData == nullptr || !Viewport->DrawData->Valid || ManagedWindow->IsMinimized())
			continue;
		const usize DrawDataIndex = Result.Windows.size();
		if (this->StateData->DrawDataScratch.size() <= DrawDataIndex)
			this->StateData->DrawDataScratch.resize(DrawDataIndex + 1U);
		if (this->StateData->DrawDataScratch[DrawDataIndex] == nullptr)
			this->StateData->DrawDataScratch[DrawDataIndex] = std::make_shared<detail::EditorUIDrawData>();
		CloneDrawDataInto(*Viewport->DrawData, *this->StateData->DrawDataScratch[DrawDataIndex]);
		Result.Windows.push_back({.Window = ManagedWindow->GetID(),
								  .ManagedWindow = ManagedWindow,
								  .Main = Viewport == DrawMainViewport,
								  .DrawData = this->StateData->DrawDataScratch[DrawDataIndex]});
	}
	return Result;
}

void EditorUserInterface::ResetProjectState()
{
	std::scoped_lock ContextLock(this->StateData->Context->GetMutex());
	ReleaseThumbnailTextures(*this->StateData);
	this->StateData->ContentBrowserStore.reset();
	this->StateData->LayoutStore.reset();
	this->StateData->MaterialSession.reset();
	this->StateData->MaterialColorEdit.reset();
	this->StateData->ReflectedPropertyEdit.reset();
	this->StateData->ContentDirectory.clear();
	this->StateData->SelectedContentPath.clear();
	this->StateData->FavoriteAssetIDs.clear();
	this->StateData->Viewports.clear();
	this->StateData->ActiveViewport = {};
	this->StateData->LayoutPrimed = false;
	this->StateData->LayoutInitialized = false;
	this->StateData->RecoveryScanned = false;
	this->StateData->PreferencesInitialized = false;
}

void EditorUserInterface::RecycleFrame(EditorUIFrame &&Frame) noexcept
{
	std::scoped_lock ContextLock(this->StateData->Context->GetMutex());
	this->StateData->FrameScratch = std::move(Frame);
}

void EditorUserInterface::Render(const EditorUIFrame &Frame, const std::span<const EditorViewportPresentation> Viewports)
{
	std::scoped_lock ContextLock(this->StateData->Context->GetMutex());
	if (Frame.Windows.empty())
		throw std::invalid_argument("Cannot render an editor UI frame without managed window draw data");
	this->StateData->Context->Activate();
	this->StateData->Renderer->BeginFrame();
	for (const EditorViewportTextureBinding &Binding : Frame.ViewportTextures)
	{
		const auto Current =
			std::ranges::find_if(Viewports, [&](const EditorViewportPresentation &Viewport) { return Viewport.View == Binding.View; });
		if (!Binding.Texture.IsValid() || Binding.Texture.ViewIdentity != Binding.View.Value)
			continue;
		const bool ReplacementValid = Current != Viewports.end() && Current->Output.Color.IsValid();
		const ImTextureID Replacement = ReplacementValid ? static_cast<ImTextureID>(Current->Output.Color.Texture) : ImTextureID_Invalid;
		for (const EditorUIWindowFrame &WindowFrame : Frame.Windows)
		{
			for (int32 ListIndex = 0; ListIndex < WindowFrame.DrawData->Data.CmdLists.Size; ++ListIndex)
			{
				ImDrawList *List = WindowFrame.DrawData->Data.CmdLists[ListIndex];
				if (List == nullptr)
					throw std::logic_error("Editor UI frame contains a null draw list");
				for (ImDrawCmd &Command : List->CmdBuffer)
				{
					if (Command.GetTexID() != static_cast<ImTextureID>(Binding.Texture.Texture))
						continue;
					Command.TexRef._TexData = nullptr;
					Command.TexRef._TexID = Replacement;
				}
			}
		}
	}
	for (const EditorUIWindowFrame &WindowFrame : Frame.Windows)
	{
		core::Window *ManagedWindow = WindowFrame.ManagedWindow;
		if (ManagedWindow == nullptr || ManagedWindow->IsMinimized())
			continue;
		core::Context &Context = ManagedWindow->GetContext();
		if (Context.IsThreadTransferPending())
			Context.AdoptCurrentThread();
		else
			Context.MakeCurrent();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDrawBuffer(GL_BACK);
		const core::WindowExtent Extent = ManagedWindow->GetFramebufferExtent();
		glViewport(0, 0, static_cast<GLsizei>(Extent.Width), static_cast<GLsizei>(Extent.Height));
		if (!WindowFrame.Main)
		{
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_STENCIL_TEST);
			glDisable(GL_SCISSOR_TEST);
			glClearColor(0.025f, 0.028f, 0.035f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		this->StateData->Renderer->Render(WindowFrame.DrawData->Data, Context);
		if (!WindowFrame.Main)
			ManagedWindow->Present();
	}
	this->StateData->Window->GetContext().MakeCurrent();
}
} // namespace editor::ui
