#pragma once
#include "src/animation/AnimationSystem.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/component/object/CObjectTransformComponent.h"
#include "src/core/layers/ApplicationLayer.h"
#include "src/core/window/Window.h"
#include "src/pipeline/device/Device.h"
#include "src/pipeline/shader/ShaderLibrary.h"
#include "src/renderer/OpenGLRenderer.h"
#include "src/resource/asset/AssetManager.h"
#include "src/scene/Camera.h"
#include "src/scene/Scene.h"

#include <memory>

namespace core
{
class RenderLayer final : public ApplicationLayer
{
  private:
	Window *Window = nullptr;
	pipeline::device::Device *Device = nullptr;
	std::unique_ptr<Camera> Camera = nullptr;
	std::unique_ptr<OpenGLRenderer> Renderer = nullptr;
	std::unique_ptr<resource::AssetManager> AssetManager = nullptr;
	std::unique_ptr<pipeline::shader::ShaderLibrary> ShaderLibrary = nullptr;
	uint32 ShadowDepthPipeline = 0;
	uint32 DepthPrepassPipeline = 0;
	uint32 GBufferPipeline = 0;
	uint32 TransparentOITPipeline = 0;
	uint32 ToneMapPipeline = 0;
	uint32 VisibilityCullPipeline = 0;
	uint32 VisibilityPrefixScanPipeline = 0;
	uint32 VisibilityBlockPrefixScanPipeline = 0;
	uint32 VisibilityFinalizePipeline = 0;
	uint32 VisibilityScatterPipeline = 0;
	uint32 HierarchicalDepthPipeline = 0;
	uint32 ClusteredLightsPipeline = 0;
	uint32 DeferredLightingPipeline = 0;
	uint32 OITCompositionPipeline = 0;
	uint32 TemporalAAPipeline = 0;
	uint32 AutoExposurePipeline = 0;
	uint32 BloomPipeline = 0;
	std::unique_ptr<world::Scene> Scene;
	animation::AnimationSystem AnimationSystem;
	world::ObjectHandle WorldObject;
	world::ComponentHandle<components::CObjectTransformComponent> WorldTransform;
	float32 RotationDegrees = 0.0f;

  public:
	RenderLayer(core::Window *Window, pipeline::device::Device &Device) : Window(Window), Device(&Device)
	{
		this->Camera = std::make_unique<::Camera>(0.1f, 90.0f, 0.1f, 5000.0f);
		// The sample mesh is centered at the origin. Start outside it and
		// orient the camera toward -Z so the first rendered frame is useful.
		this->Camera->Position = glm::vec3(0.0f, 0.0f, 5.0f);
		this->Camera->Yaw = -90.0f;
		this->Camera->Pitch = 0.0f;
		this->Camera->UpdateCameraVectors();
		this->Renderer = std::make_unique<OpenGLRenderer>(Device);
		this->AssetManager = std::make_unique<resource::AssetManager>();
		this->ShaderLibrary = std::make_unique<pipeline::shader::ShaderLibrary>(Device, *this->AssetManager);
		this->ShadowDepthPipeline = this->ShaderLibrary->CreateGraphicsPipeline(
			{.Vertex = {.Path = "shader/ShadowDepth.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
			 .Fragment = {.Path = "shader/ShadowDepth.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
			 .State = {.DepthStencil = {.DepthTest = true, .DepthWrite = true, .DepthCompare = pipeline::shader::CompareFunction::Less},
					   .RenderTargets = {.ColorAttachmentCount = 0, .HasDepth = true}}});
		this->DepthPrepassPipeline = this->ShaderLibrary->CreateGraphicsPipeline(
			{.Vertex = {.Path = "shader/GBuffer.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
			 .Fragment = {.Path = "shader/DepthMasked.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
			 .State = {.DepthStencil = {.DepthTest = true, .DepthWrite = true, .DepthCompare = pipeline::shader::CompareFunction::Greater},
					   .RenderTargets = {.ColorAttachmentCount = 0, .HasDepth = true}}});
		this->GBufferPipeline = this->ShaderLibrary->CreateGraphicsPipeline(
			{.Vertex = {.Path = "shader/GBuffer.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
			 .Fragment = {.Path = "shader/GBuffer.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
			 .State = {
				 .DepthStencil = {.DepthTest = true, .DepthWrite = false, .DepthCompare = pipeline::shader::CompareFunction::GreaterEqual},
				 .RenderTargets = {.ColorAttachmentCount = 5, .HasDepth = true}}});
		this->TransparentOITPipeline = this->ShaderLibrary->CreateGraphicsPipeline(
			{.Vertex = {.Path = "shader/GBuffer.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
			 .Fragment = {.Path = "shader/TransparentOIT.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
			 .State = {.DepthStencil = {.DepthTest = true, .DepthWrite = false},
					   .ColorAttachmentBlends = {{.Enabled = true,
												  .SourceColor = pipeline::shader::BlendFactor::One,
												  .DestinationColor = pipeline::shader::BlendFactor::One,
												  .SourceAlpha = pipeline::shader::BlendFactor::One,
												  .DestinationAlpha = pipeline::shader::BlendFactor::One},
												 {.Enabled = true,
												  .SourceColor = pipeline::shader::BlendFactor::Zero,
												  .DestinationColor = pipeline::shader::BlendFactor::OneMinusSourceColor,
												  .SourceAlpha = pipeline::shader::BlendFactor::Zero,
												  .DestinationAlpha = pipeline::shader::BlendFactor::OneMinusSourceAlpha}},
					   .RenderTargets = {.ColorAttachmentCount = 2, .HasDepth = true}}});
		pipeline::shader::ShaderPermutationKey ToneMapPermutation;
		ToneMapPermutation.Set(pipeline::shader::ShaderFeature::ManualSRGBEncode, !this->Window->IsSRGBPresentationCapable());
		this->ToneMapPipeline = this->ShaderLibrary->CreateGraphicsPipeline(
			{.Vertex = {.Path = "shader/Fullscreen.vert", .Stage = pipeline::shader::ShaderStage::Vertex},
			 .Fragment = {.Path = "shader/ToneMap.frag", .Stage = pipeline::shader::ShaderStage::Fragment},
			 .Permutation = ToneMapPermutation,
			 .State = {.Rasterizer = {.CullMode = pipeline::shader::CullMode::None},
					   .DepthStencil = {.DepthTest = false, .DepthWrite = false},
					   .RenderTargets = {.ColorAttachmentCount = 1, .HasDepth = false}}});
		this->VisibilityCullPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/Visibility.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->VisibilityPrefixScanPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/VisibilityPrefixScan.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->VisibilityBlockPrefixScanPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/VisibilityBlockPrefixScan.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->VisibilityFinalizePipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/VisibilityFinalize.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->VisibilityScatterPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/VisibilityScatter.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->HierarchicalDepthPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/HiZ.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->ClusteredLightsPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/ClusteredLights.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->DeferredLightingPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/DeferredLighting.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->OITCompositionPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/OITComposite.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->TemporalAAPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/TAA.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->AutoExposurePipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/AutoExposure.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->BloomPipeline = this->ShaderLibrary->CreateComputePipeline(
			{.Compute = {.Path = "shader/Bloom.comp", .Stage = pipeline::shader::ShaderStage::Compute}});
		this->Scene = std::make_unique<world::Scene>();
		this->WorldObject = this->Scene->CreateObject();
		this->WorldTransform = this->Scene->AddComponent<components::CObjectTransformComponent>(this->WorldObject);
		auto Model = this->AssetManager->GetAsset<resource::ModelAsset>("objects/backpack.obj");
		(void)this->Scene->AddComponent<components::CObjectMeshComponent>(this->WorldObject, std::move(Model));
	}

	virtual ~RenderLayer() override
	{
	}

	virtual void Run(const ApplicationFrame &Frame) override
	{
		if (Frame.Input == nullptr)
			throw std::logic_error("RenderLayer requires an InputSnapshot");
		this->ShaderLibrary->BeginFrame();
		static PointLightSource PointLight{
			glm::vec3(0.0f, -10.0f, 0.0f),
			// AMBIENT: Extremely low, neutral grey.
			// This prevents the "red glow" in the dark from your previous settings.
			glm::vec3(0.02f, 0.02f, 0.02f),

			// DIFFUSE: Warm Light Bulb (Kelvin ~2700K - 3000K)
			// R: 1.0, G: 0.85, B: 0.65 creates a natural, cozy indoor glow.
			glm::vec3(1.0f, 0.85f, 0.65f),

			// SPECULAR: Keep this bright/white for the "shine" on surfaces.
			glm::vec3(1.0f, 1.0f, 1.0f),

			1.0f,  // Constant
			0.09f, // Linear
			0.032f // Quadratic (This preset is great for a range of ~50 units)
		};

		static SpotLightSource SpotLight{glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::cos(glm::radians(12.5f)),
										 glm::cos(glm::radians(17.5f)),
										 // AMBIENT: Set to 0.0f. Spotlights should usually NOT light up the darkness behind objects.
										 glm::vec3(0.0f, 0.0f, 0.0f),

										 // DIFFUSE: Warm Tungsten
										 glm::vec3(1.0f, 0.95f, 0.8f),
										 // SPECULAR: Bright white
										 glm::vec3(1.0f, 1.0f, 1.0f), 1.0f, 0.001f, 0.00001f};
		SpotLight.LookAt(glm::vec3(0.0f, 0.0f, 0.0f));

		static DirectionalLightSource DirL{glm::vec3(0.5f, -0.7f, 0.5f),
										   // AMBIENT: Extremely low (0.02). This ensures back-faces are nearly black,
										   // but not "void" black (so you can essentially only see the lit side).
										   glm::vec3(0.02f, 0.02f, 0.02f),

										   // DIFFUSE: Soft Sunlight
										   glm::vec3(0.8f, 0.8f, 0.75f),
										   // SPECULAR
										   glm::vec3(1.0f, 1.0f, 1.0f)};

		static std::vector<PointLightSource> PointLights = {PointLight};
		static std::vector<SpotLightSource> SpotLights = {SpotLight};
		static std::vector<DirectionalLightSource> DirectionalLights = {DirL};

		this->Renderer->UploadLightSources(PointLights);
		this->Renderer->UploadLightSources(SpotLights);
		this->Renderer->UploadLightSources(DirectionalLights);

		this->Camera->Update(*Frame.Input, static_cast<float32>(Frame.Timing.DeltaSeconds));
		this->AnimationSystem.Update(*this->Scene, static_cast<float32>(Frame.Timing.DeltaSeconds));
		this->RotationDegrees += 90.0f * static_cast<float32>(Frame.Timing.DeltaSeconds);

		{
			auto Access = this->Scene->Write();
			Access.Resolve(this->WorldTransform).SetRotationEuler(glm::vec3(this->RotationDegrees, 0.0f, 0.0f));
		}

		this->Camera->UpdateCameraVectors();
		this->Renderer->Render(*this->Scene, *this->AssetManager, *this->Camera);
		const renderer::RenderPassPipelineSet Pipelines{
			.ShadowDepth = this->ShaderLibrary->GetGraphicsPipeline(this->ShadowDepthPipeline),
			.DepthPrepass = this->ShaderLibrary->GetGraphicsPipeline(this->DepthPrepassPipeline),
			.GBuffer = this->ShaderLibrary->GetGraphicsPipeline(this->GBufferPipeline),
			.TransparentOIT = this->ShaderLibrary->GetGraphicsPipeline(this->TransparentOITPipeline),
			.ToneMap = this->ShaderLibrary->GetGraphicsPipeline(this->ToneMapPipeline),
			.VisibilityCull = this->ShaderLibrary->GetComputePipeline(this->VisibilityCullPipeline),
			.VisibilityPrefixScan = this->ShaderLibrary->GetComputePipeline(this->VisibilityPrefixScanPipeline),
			.VisibilityBlockPrefixScan = this->ShaderLibrary->GetComputePipeline(this->VisibilityBlockPrefixScanPipeline),
			.VisibilityFinalize = this->ShaderLibrary->GetComputePipeline(this->VisibilityFinalizePipeline),
			.VisibilityScatter = this->ShaderLibrary->GetComputePipeline(this->VisibilityScatterPipeline),
			.HierarchicalDepth = this->ShaderLibrary->GetComputePipeline(this->HierarchicalDepthPipeline),
			.ClusteredLights = this->ShaderLibrary->GetComputePipeline(this->ClusteredLightsPipeline),
			.DeferredLighting = this->ShaderLibrary->GetComputePipeline(this->DeferredLightingPipeline),
			.OITComposition = this->ShaderLibrary->GetComputePipeline(this->OITCompositionPipeline),
			.TemporalAA = this->ShaderLibrary->GetComputePipeline(this->TemporalAAPipeline),
			.AutoExposure = this->ShaderLibrary->GetComputePipeline(this->AutoExposurePipeline),
			.Bloom = this->ShaderLibrary->GetComputePipeline(this->BloomPipeline)};
		this->Renderer->RenderScene(Pipelines, *this->Camera, *this->Window);
	};
};
} // namespace core
