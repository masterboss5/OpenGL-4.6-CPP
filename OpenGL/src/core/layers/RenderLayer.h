#pragma once
#include "src/core/layers/ApplicationLayer.h"
#include "src/core/input/Window.h"
#include "src/scene/Camera.h"
#include "src/scene/StaticMeshObject.h"
#include "src/renderer/OpenGLRenderer.h"
#include "src/resource/FileLoader.h"
#include "src/resource/asset/AssetManager.h"
#include "src/pipeline/shader/ShaderLibrary.h"
#include <memory>

namespace core
{
	class RenderLayer final : public ApplicationLayer
	{
	private:
		const Window* window = nullptr;
		std::unique_ptr<Camera> camera = nullptr;
		std::unique_ptr<OpenGLRenderer> openGLRenderer = nullptr;
		std::unique_ptr<resource::AssetManager> assetManager = nullptr;
		std::unique_ptr<pipeline::shader::ShaderLibrary> shaderLibrary = nullptr;
		uint32 shadowDepthPipeline = 0;
		uint32 depthPrepassPipeline = 0;
		uint32 gbufferPipeline = 0;
		uint32 transparentOITPipeline = 0;
		uint32 toneMapPipeline = 0;
		uint32 visibilityCullPipeline = 0;
		uint32 visibilityPrefixScanPipeline = 0;
		uint32 visibilityBlockPrefixScanPipeline = 0;
		uint32 visibilityFinalizePipeline = 0;
		uint32 visibilityScatterPipeline = 0;
		uint32 hierarchicalDepthPipeline = 0;
		uint32 clusteredLightsPipeline = 0;
		uint32 deferredLightingPipeline = 0;
		uint32 oitCompositionPipeline = 0;
		uint32 temporalAAPipeline = 0;
		uint32 autoExposurePipeline = 0;
		uint32 bloomPipeline = 0;
	public:
		RenderLayer(const Window* window) 
			: window(window)
		{
			this->camera = std::make_unique<Camera>(0.1f, 90.0f, 0.1f, 5000.0f);
			// The sample mesh is centered at the origin. Start outside it and
			// orient the camera toward -Z so the first rendered frame is useful.
			this->camera->position = glm::vec3(0.0f, 0.0f, 5.0f);
			this->camera->yaw = -90.0f;
			this->camera->pitch = 0.0f;
			this->camera->updateCameraVectors();
			this->openGLRenderer = std::make_unique<OpenGLRenderer>();
			this->assetManager = std::make_unique<resource::AssetManager>();
			this->shaderLibrary = std::make_unique<pipeline::shader::ShaderLibrary>(*this->assetManager);
			this->shadowDepthPipeline = this->shaderLibrary->createGraphicsPipeline({ .vertex = { .path = "shader/ShadowDepth.vert", .stage = pipeline::shader::ShaderStage::Vertex }, .fragment = { .path = "shader/DepthOnly.frag", .stage = pipeline::shader::ShaderStage::Fragment }, .state = { .depthStencil = { .depthTest = true, .depthWrite = true, .depthCompare = pipeline::shader::CompareFunction::Less }, .renderTargets = { .colorAttachmentCount = 0, .hasDepth = true } } });
			this->depthPrepassPipeline = this->shaderLibrary->createGraphicsPipeline({ .vertex = { .path = "shader/GBuffer.vert", .stage = pipeline::shader::ShaderStage::Vertex }, .fragment = { .path = "shader/DepthOnly.frag", .stage = pipeline::shader::ShaderStage::Fragment }, .state = { .depthStencil = { .depthTest = true, .depthWrite = true, .depthCompare = pipeline::shader::CompareFunction::Greater }, .renderTargets = { .colorAttachmentCount = 0, .hasDepth = true } } });
			this->gbufferPipeline = this->shaderLibrary->createGraphicsPipeline({ .vertex = { .path = "shader/GBuffer.vert", .stage = pipeline::shader::ShaderStage::Vertex }, .fragment = { .path = "shader/GBuffer.frag", .stage = pipeline::shader::ShaderStage::Fragment }, .state = { .depthStencil = { .depthTest = true, .depthWrite = false, .depthCompare = pipeline::shader::CompareFunction::GreaterEqual }, .renderTargets = { .colorAttachmentCount = 5, .hasDepth = true } } });
			this->transparentOITPipeline = this->shaderLibrary->createGraphicsPipeline({ .vertex = { .path = "shader/GBuffer.vert", .stage = pipeline::shader::ShaderStage::Vertex }, .fragment = { .path = "shader/TransparentOIT.frag", .stage = pipeline::shader::ShaderStage::Fragment }, .state = { .depthStencil = { .depthTest = true, .depthWrite = false }, .colorAttachmentBlends = { { .enabled = true, .sourceColor = pipeline::shader::BlendFactor::One, .destinationColor = pipeline::shader::BlendFactor::One, .sourceAlpha = pipeline::shader::BlendFactor::One, .destinationAlpha = pipeline::shader::BlendFactor::One }, { .enabled = true, .sourceColor = pipeline::shader::BlendFactor::Zero, .destinationColor = pipeline::shader::BlendFactor::OneMinusSourceColor, .sourceAlpha = pipeline::shader::BlendFactor::Zero, .destinationAlpha = pipeline::shader::BlendFactor::OneMinusSourceAlpha } }, .renderTargets = { .colorAttachmentCount = 2, .hasDepth = true } } });
			this->toneMapPipeline = this->shaderLibrary->createGraphicsPipeline({ .vertex = { .path = "shader/Fullscreen.vert", .stage = pipeline::shader::ShaderStage::Vertex }, .fragment = { .path = "shader/ToneMap.frag", .stage = pipeline::shader::ShaderStage::Fragment }, .state = { .rasterizer = { .cullMode = pipeline::shader::CullMode::None }, .depthStencil = { .depthTest = false, .depthWrite = false }, .renderTargets = { .colorAttachmentCount = 1, .hasDepth = false } } });
			this->visibilityCullPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/Visibility.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->visibilityPrefixScanPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/VisibilityPrefixScan.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->visibilityBlockPrefixScanPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/VisibilityBlockPrefixScan.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->visibilityFinalizePipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/VisibilityFinalize.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->visibilityScatterPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/VisibilityScatter.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->hierarchicalDepthPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/HiZ.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->clusteredLightsPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/ClusteredLights.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->deferredLightingPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/DeferredLighting.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->oitCompositionPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/OITComposite.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->temporalAAPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/TAA.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->autoExposurePipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/AutoExposure.comp", .stage = pipeline::shader::ShaderStage::Compute } });
			this->bloomPipeline = this->shaderLibrary->createComputePipeline({ .compute = { .path = "shader/Bloom.comp", .stage = pipeline::shader::ShaderStage::Compute } });
		}

		virtual ~RenderLayer() override
		{
		}

		virtual void run() override
		{
			this->shaderLibrary->beginFrame();
			static StaticMesh* worldObject = FileLoader::readObj("objects/backpack.obj", "objects");
			static StaticMeshObject backPack(worldObject, 0, 0, 0);


			static PointLightSource pointLight {
	glm::vec3(0.0f, -10.0f, 0.0f),
	// AMBIENT: Extremely low, neutral grey. 
	// This prevents the "red glow" in the dark from your previous settings.
	glm::vec3(0.02f, 0.02f, 0.02f),

	// DIFFUSE: Warm Light Bulb (Kelvin ~2700K - 3000K)
	// R: 1.0, G: 0.85, B: 0.65 creates a natural, cozy indoor glow.
	glm::vec3(1.0f, 0.85f, 0.65f),

	// SPECULAR: Keep this bright/white for the "shine" on surfaces.
	glm::vec3(1.0f, 1.0f, 1.0f),

	1.0f,    // Constant
	0.09f,   // Linear
	0.032f   // Quadratic (This preset is great for a range of ~50 units)
			};

			static SpotLightSource spotLight {
				glm::vec3(0.0f, 10.0f, 0.0f),
				glm::vec3(0.0f, 0.0f, 0.0f),
				glm::cos(glm::radians(12.5f)),
				glm::cos(glm::radians(17.5f)),
				// AMBIENT: Set to 0.0f. Spotlights should usually NOT light up the darkness behind objects.
				glm::vec3(0.0f, 0.0f, 0.0f),

				// DIFFUSE: Warm Tungsten
				glm::vec3(1.0f, 0.95f, 0.8f),
				// SPECULAR: Bright white
				glm::vec3(1.0f, 1.0f, 1.0f),
				1.0f,
				0.001f,
				0.00001f
			};
			spotLight.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));

			static DirectionalLightSource dirL {
				glm::vec3(0.5f, -0.7f, 0.5f),
				// AMBIENT: Extremely low (0.02). This ensures back-faces are nearly black, 
				// but not "void" black (so you can essentially only see the lit side).
				glm::vec3(0.02f, 0.02f, 0.02f),

				// DIFFUSE: Soft Sunlight
				glm::vec3(0.8f, 0.8f, 0.75f),
				// SPECULAR
				glm::vec3(1.0f, 1.0f, 1.0f)
			};

			static std::vector<PointLightSource> pointLights = { pointLight };
			static std::vector<SpotLightSource> spotLights = { spotLight };
			static std::vector<DirectionalLightSource> directionalLights = { dirL };

			this->openGLRenderer->uploadLightSources(pointLights);
			this->openGLRenderer->uploadLightSources(spotLights);
			this->openGLRenderer->uploadLightSources(directionalLights);

			this->camera->update(*this->window, this->window->getFrameDeltaTime());
			static float rotX = 0.0f;
			rotX += 90.0f * window->getFrameDeltaTime();

			backPack.setRotation(rotX, 0.0f, 0.0f);

			this->camera->updateCameraVectors();
			this->openGLRenderer->render(backPack);
			const renderer::RenderPassPipelineSet pipelines { .shadowDepth = this->shaderLibrary->getGraphicsPipeline(this->shadowDepthPipeline), .depthPrepass = this->shaderLibrary->getGraphicsPipeline(this->depthPrepassPipeline), .gbuffer = this->shaderLibrary->getGraphicsPipeline(this->gbufferPipeline), .transparentOIT = this->shaderLibrary->getGraphicsPipeline(this->transparentOITPipeline), .toneMap = this->shaderLibrary->getGraphicsPipeline(this->toneMapPipeline), .visibilityCull = this->shaderLibrary->getComputePipeline(this->visibilityCullPipeline), .visibilityPrefixScan = this->shaderLibrary->getComputePipeline(this->visibilityPrefixScanPipeline), .visibilityBlockPrefixScan = this->shaderLibrary->getComputePipeline(this->visibilityBlockPrefixScanPipeline), .visibilityFinalize = this->shaderLibrary->getComputePipeline(this->visibilityFinalizePipeline), .visibilityScatter = this->shaderLibrary->getComputePipeline(this->visibilityScatterPipeline), .hierarchicalDepth = this->shaderLibrary->getComputePipeline(this->hierarchicalDepthPipeline), .clusteredLights = this->shaderLibrary->getComputePipeline(this->clusteredLightsPipeline), .deferredLighting = this->shaderLibrary->getComputePipeline(this->deferredLightingPipeline), .oitComposition = this->shaderLibrary->getComputePipeline(this->oitCompositionPipeline), .temporalAA = this->shaderLibrary->getComputePipeline(this->temporalAAPipeline), .autoExposure = this->shaderLibrary->getComputePipeline(this->autoExposurePipeline), .bloom = this->shaderLibrary->getComputePipeline(this->bloomPipeline) };
			this->openGLRenderer->renderScene(pipelines, *this->camera, *this->window);
		};
	};
}
