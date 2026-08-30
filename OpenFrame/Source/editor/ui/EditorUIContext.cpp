#include "EditorUIContext.h"

#include "EditorStyle.h"

#include <imgui.h>

#include <filesystem>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace editor::ui
{
namespace
{
[[nodiscard]] std::filesystem::path GetSystemFontPath(const std::filesystem::path &FileName)
{
	std::vector<wchar_t> Buffer(32'768);
	const DWORD Length = GetWindowsDirectoryW(Buffer.data(), static_cast<DWORD>(Buffer.size()));
	if (Length == 0 || Length >= Buffer.size())
		throw std::runtime_error("Could not resolve the system font directory");
	return std::filesystem::path(Buffer.data(), Buffer.data() + Length) / "Fonts" / FileName;
}

void ConfigureFonts(ImGuiIO &IO)
{
	const std::filesystem::path InterfaceFont = GetSystemFontPath("segoeui.ttf");
	if (!std::filesystem::is_regular_file(InterfaceFont))
		throw std::runtime_error("The editor requires the Segoe UI system font");
	ImFontConfig InterfaceConfiguration;
	InterfaceConfiguration.OversampleH = 2;
	InterfaceConfiguration.OversampleV = 2;
	InterfaceConfiguration.PixelSnapH = true;
	IO.FontDefault = IO.Fonts->AddFontFromFileTTF(InterfaceFont.string().c_str(), 14.0F, &InterfaceConfiguration);
	if (IO.FontDefault == nullptr)
		throw std::runtime_error("Dear ImGui could not load the editor interface font");

	std::filesystem::path IconFont = GetSystemFontPath("SegoeIcons.ttf");
	if (!std::filesystem::is_regular_file(IconFont))
		IconFont = GetSystemFontPath("segmdl2.ttf");
	if (std::filesystem::is_regular_file(IconFont))
	{
		static constexpr ImWchar IconRanges[]{0xE000, 0xF8FF, 0};
		ImFontConfig IconConfiguration;
		IconConfiguration.MergeMode = true;
		IconConfiguration.PixelSnapH = true;
		IconConfiguration.GlyphMinAdvanceX = 16.0F;
		if (IO.Fonts->AddFontFromFileTTF(IconFont.string().c_str(), 16.0F, &IconConfiguration, IconRanges) == nullptr)
			throw std::runtime_error("Dear ImGui could not load the editor icon font");
	}
}
} // namespace

EditorUIContext::EditorUIContext()
{
	IMGUI_CHECKVERSION();
	this->Context = ImGui::CreateContext();
	if (this->Context == nullptr)
		throw std::runtime_error("Dear ImGui failed to create the editor interface context");
	try
	{
		this->Activate();
		ImGuiIO &IO = ImGui::GetIO();
		IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
		IO.ConfigWindowsMoveFromTitleBarOnly = true;
		IO.IniFilename = nullptr;
		IO.LogFilename = nullptr;
		ConfigureFonts(IO);
		EditorStyleSystem::ApplyDefaultDark();
	}
	catch (...)
	{
		ImGui::DestroyContext(this->Context);
		this->Context = nullptr;
		throw;
	}
}

EditorUIContext::~EditorUIContext()
{
	if (this->Context == nullptr)
		return;
	this->Activate();
	ImGui::DestroyContext(this->Context);
	this->Context = nullptr;
}

void EditorUIContext::Activate() const noexcept
{
	ImGui::SetCurrentContext(this->Context);
}

std::mutex &EditorUIContext::GetMutex() noexcept
{
	return this->Mutex;
}

ImGuiContext *EditorUIContext::GetNativeContext() const noexcept
{
	return this->Context;
}
} // namespace editor::ui
