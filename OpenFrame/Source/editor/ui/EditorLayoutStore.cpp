#include "EditorLayoutStore.h"

#include "Source/core/io/SecurePath.h"
#include "Source/util/UUID.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace editor::ui
{
namespace
{
using Json = nlohmann::json;

[[nodiscard]] std::span<const uint8> BytesOf(const string &Text) noexcept
{
	return {reinterpret_cast<const uint8 *>(Text.data()), Text.size()};
}
} // namespace

EditorLayoutStore::EditorLayoutStore(std::filesystem::path Path) : Path(std::filesystem::absolute(std::move(Path)).lexically_normal())
{
	if (this->Path.empty())
		throw std::invalid_argument("Editor layout store requires a path");
}

bool EditorLayoutStore::Load(workspace::EditorWorkspace &Workspace, string &DockingState, const uint32 CurrentWidth,
							 const uint32 CurrentHeight, std::vector<EditorViewportLayoutState> *ViewportStates)
{
	if (!std::filesystem::is_regular_file(this->Path))
		return false;
	try
	{
		constexpr uint64 MaximumLayoutBytes = 16U * 1024U * 1024U;
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(this->Path.parent_path(), this->Path.filename(), MaximumLayoutBytes, "editor layout");
		const Json Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
		if (!Root.is_object())
			throw std::runtime_error("layout root must be an object");
		const uint32 FormatVersion = Root.at("FormatVersion").get<uint32>();
		if (FormatVersion < 2U || FormatVersion > CurrentFormatVersion || !Root.at("Panels").is_array())
		{
			throw std::runtime_error("layout root, version, or panel table is invalid");
		}
		for (const Json &Panel : Root.at("Panels"))
		{
			const auto ID = static_cast<workspace::EditorPanelID>(Panel.at("ID").get<uint32>());
			if (static_cast<uint32>(ID) >= static_cast<uint32>(workspace::EditorPanelID::Count))
				throw std::runtime_error("layout contains an invalid panel ID");
			workspace::EditorPanelState &State = Workspace.GetPanel(ID);
			State.Open = Panel.at("Open").get<bool>();
			if (!State.Closable)
				State.Open = true;
		}
		const uint32 StoredWidth = Root.value("Width", 0U);
		const uint32 StoredHeight = Root.value("Height", 0U);
		const bool CompatibleExtent = CurrentWidth == 0 || CurrentHeight == 0 || StoredWidth == 0 || StoredHeight == 0 ||
									  (std::abs(static_cast<float64>(StoredWidth) / static_cast<float64>(CurrentWidth) - 1.0) <= 0.1 &&
									   std::abs(static_cast<float64>(StoredHeight) / static_cast<float64>(CurrentHeight) - 1.0) <= 0.1);
		DockingState = CompatibleExtent ? Root.value("DockingState", string{}) : string{};
		std::vector<EditorViewportLayoutState> LoadedViewports;
		if (FormatVersion >= 3U)
		{
			for (const Json &Viewport : Root.value("Viewports", Json::array()))
			{
				const uint64 View = Viewport.at("View").get<uint64>();
				const uint32 Mode = Viewport.at("ViewMode").get<uint32>();
				if (View == 0 || Mode >= static_cast<uint32>(pipeline::render::ViewportViewMode::Count))
					throw std::runtime_error("layout contains invalid viewport settings");
				const Json &Overlays = Viewport.at("Overlays");
				LoadedViewports.push_back({.View = View,
										   .Settings = {.ViewMode = static_cast<pipeline::render::ViewportViewMode>(Mode),
														.Overlays = {.Grid = Overlays.value("Grid", true),
																	 .Bounds = Overlays.value("Bounds", false),
																	 .Skeletons = Overlays.value("Skeletons", false),
																	 .Cameras = Overlays.value("Cameras", false),
																	 .Lights = Overlays.value("Lights", false),
																	 .Culling = Overlays.value("Culling", false),
																	 .Selection = Overlays.value("Selection", true),
																	 .RenderStatistics = Overlays.value("RenderStatistics", false),
																	 .RenderGraph = Overlays.value("RenderGraph", false)}}});
			}
		}
		this->Capture(Workspace.GetPanels(), DockingState, CurrentWidth, CurrentHeight, LoadedViewports);
		this->Dirty = false;
		if (ViewportStates != nullptr)
			*ViewportStates = std::move(LoadedViewports);
		return true;
	}
	catch (const std::exception &Exception)
	{
		throw std::runtime_error("Could not load editor layout '" + this->Path.string() + "': " + Exception.what());
	}
}

void EditorLayoutStore::Capture(const std::span<const workspace::EditorPanelState> Panels, const string_view DockingState,
								const uint32 CurrentWidth, const uint32 CurrentHeight,
								const std::span<const EditorViewportLayoutState> ViewportStates)
{
	bool Changed = string_view(this->DockingState) != DockingState || this->Panels.size() != Panels.size() || this->Width != CurrentWidth ||
				   this->Height != CurrentHeight || this->ViewportStates.size() != ViewportStates.size();
	if (!Changed)
	{
		for (usize Index = 0; Index < Panels.size(); ++Index)
		{
			if (this->Panels[Index].ID != Panels[Index].ID || this->Panels[Index].Open != Panels[Index].Open)
			{
				Changed = true;
				break;
			}
		}
	}
	if (!Changed)
	{
		for (usize Index = 0; Index < ViewportStates.size(); ++Index)
		{
			if (this->ViewportStates[Index].View != ViewportStates[Index].View ||
				this->ViewportStates[Index].Settings != ViewportStates[Index].Settings)
			{
				Changed = true;
				break;
			}
		}
	}
	if (!Changed)
		return;
	this->Panels.assign(Panels.begin(), Panels.end());
	this->DockingState.assign(DockingState);
	this->ViewportStates.assign(ViewportStates.begin(), ViewportStates.end());
	this->Width = CurrentWidth;
	this->Height = CurrentHeight;
	this->Dirty = true;
}

void EditorLayoutStore::Flush()
{
	if (!this->Dirty)
		return;
	Json PanelTable = Json::array();
	for (const workspace::EditorPanelState &Panel : this->Panels)
		PanelTable.push_back({{"ID", static_cast<uint32>(Panel.ID)}, {"Open", Panel.Open}});
	Json ViewportTable = Json::array();
	for (const EditorViewportLayoutState &Viewport : this->ViewportStates)
	{
		const pipeline::render::ViewportOverlaySettings &Overlays = Viewport.Settings.Overlays;
		ViewportTable.push_back({{"View", Viewport.View},
								 {"ViewMode", static_cast<uint32>(Viewport.Settings.ViewMode)},
								 {"Overlays",
								  {{"Grid", Overlays.Grid},
								   {"Bounds", Overlays.Bounds},
								   {"Skeletons", Overlays.Skeletons},
								   {"Cameras", Overlays.Cameras},
								   {"Lights", Overlays.Lights},
								   {"Culling", Overlays.Culling},
								   {"Selection", Overlays.Selection},
								   {"RenderStatistics", Overlays.RenderStatistics},
								   {"RenderGraph", Overlays.RenderGraph}}}});
	}
	const Json Root{{"FormatVersion", CurrentFormatVersion},
					{"Width", this->Width},
					{"Height", this->Height},
					{"DockingState", this->DockingState},
					{"Panels", std::move(PanelTable)},
					{"Viewports", std::move(ViewportTable)}};
	const std::filesystem::path DirectoryRoot = this->Path.parent_path();
	const std::filesystem::path Destination = this->Path.filename();
	const std::filesystem::path Temporary = Destination.string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	const string Serialized = Root.dump(2) + '\n';
	core::io::SecurePath::CreateTrustedRoot(DirectoryRoot, "editor layout root");
	core::io::SecurePath::WriteFileWithin(DirectoryRoot, Temporary, BytesOf(Serialized), false, true, "editor layout temporary file");
	try
	{
		core::io::SecurePath::ReplaceWithin(DirectoryRoot, Temporary, Destination, "editor layout publication");
	}
	catch (...)
	{
		try
		{
			core::io::SecurePath::RemoveWithin(DirectoryRoot, Temporary, false, "editor layout temporary cleanup");
		}
		catch (...)
		{
		}
		throw;
	}
	this->Dirty = false;
}

const std::filesystem::path &EditorLayoutStore::GetPath() const noexcept
{
	return this->Path;
}

bool EditorLayoutStore::IsDirty() const noexcept
{
	return this->Dirty;
}
} // namespace editor::ui
