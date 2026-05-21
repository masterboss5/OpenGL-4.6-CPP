#pragma once
#include "src/core/layers/ApplicationLayer.h"
#include "src/core/input/Window.h"
#include "src/scene/Camera.h"
#include "src/renderer/OpenGLRenderer.h"
#include "src/resource/FileLoader.h"
#include <memory>

namespace core
{
	class RenderLayer final : public ApplicationLayer
	{
	private:
		const Window* window = nullptr;
		std::unique_ptr<Camera> camera = nullptr;
		std::unique_ptr<OpenGLRenderer> openGLRenderer = nullptr;
		std::unique_ptr<ShaderProgram> shaderProgram = nullptr;
	public:
		RenderLayer(const Window* window) 
			: window(window)
		{
			this->camera = std::make_unique<Camera>(0.1, 90.0f, 0.1f, 5000.0f);
			this->openGLRenderer = std::make_unique<OpenGLRenderer>();
			this->shaderProgram = std::make_unique<ShaderProgram>("shader/VertexShader.glsl", "shader/FragmentShader.glsl");
			this->shaderProgram->bind();
		}

		virtual ~RenderLayer() override
		{
		}

		virtual void run() override
		{
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
			static std::vector<DirectionalLightSource> directionalLights = {};

			this->openGLRenderer->uploadLightSources(pointLights, *this->shaderProgram);
			this->openGLRenderer->uploadLightSources(spotLights, *this->shaderProgram);
			this->openGLRenderer->uploadLightSources(directionalLights, *this->shaderProgram);

			this->camera->update(*this->window, this->window->getFrameDeltaTime());
			static float rotX = 0.0f;
			rotX += 90.0f * window->getFrameDeltaTime();

			backPack.setRotation(rotX, 0.0f, 0.0f);

			this->camera->updateCameraVectors();
			this->openGLRenderer->render(backPack);
			this->openGLRenderer->renderScene(*this->shaderProgram, *this->camera, *this->window);
		};
	};
}