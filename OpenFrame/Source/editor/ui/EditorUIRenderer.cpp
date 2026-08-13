#include "EditorUIRenderer.h"

#include "EditorUIContext.h"
#include "Source/core/window/Context.h"

#include <imgui_impl_opengl3.h>

#include <stdexcept>

namespace editor::ui
{
EditorUIRenderer::EditorUIRenderer(EditorUIContext &Context) noexcept : Context(&Context)
{
}

EditorUIRenderer::~EditorUIRenderer()
{
	if (this->Initialized)
		std::terminate();
}

void EditorUIRenderer::Initialize()
{
	this->Context->Activate();
	if (this->Initialized)
		return;
	if (!ImGui_ImplOpenGL3_Init("#version 460 core"))
		throw std::runtime_error("Dear ImGui failed to initialize its OpenGL renderer");
	try
	{
		ImGui_ImplOpenGL3_NewFrame();
		for (ImTextureData *Texture : ImGui::GetPlatformIO().Textures)
		{
			if (Texture->Status != ImTextureStatus_OK)
				ImGui_ImplOpenGL3_UpdateTexture(Texture);
		}
		this->Initialized = true;
	}
	catch (...)
	{
		ImGui_ImplOpenGL3_Shutdown();
		throw;
	}
}

void EditorUIRenderer::Shutdown() noexcept
{
	if (!this->Initialized)
		return;
	this->Context->Activate();
	ImGui_ImplOpenGL3_Shutdown();
	this->Initialized = false;
}

void EditorUIRenderer::Render(ImDrawData &DrawData)
{
	if (!this->Initialized)
		throw std::logic_error("Dear ImGui renderer is not initialized");
	this->Context->Activate();
	ImGui_ImplOpenGL3_RenderDrawData(&DrawData);
}

void EditorUIRenderer::Render(ImDrawData &DrawData, core::Context &CurrentContext)
{
	if (!this->Initialized)
		throw std::logic_error("Dear ImGui renderer is not initialized");
	CurrentContext.RequireCurrentThread();
	ImGui_ImplOpenGL3_RenderDrawData(&DrawData);
}

void EditorUIRenderer::BeginFrame()
{
	if (!this->Initialized)
		throw std::logic_error("Dear ImGui renderer is not initialized");
	this->Context->Activate();
	for (ImTextureData *Texture : ImGui::GetPlatformIO().Textures)
	{
		if (Texture->Status != ImTextureStatus_OK)
			ImGui_ImplOpenGL3_UpdateTexture(Texture);
	}
	ImGui_ImplOpenGL3_NewFrame();
}

bool EditorUIRenderer::IsInitialized() const noexcept
{
	return this->Initialized;
}
} // namespace editor::ui
