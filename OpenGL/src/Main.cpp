#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Window.h"
#include "StaticMesh.h"
#include <vector>
#include "ShaderProgram.h"
#include <iostream>
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include "OpenGLRenderer.h"
#include "Texture.h"
#include "FileLoader.h"
#include "SpotLightSource.h"
#include "Camera.h"
#include "Application.h"
#include "core/layers/RenderLayer.h"

#define FAR_PLANE 600.0f
#define NEAR_PLANE 0.1f

#define RESOLUTION_1080P(spec) \
    spec.width = 1920;         \
    spec.height = 1080;

#define RESOLUTION_1440P(spec) \
    spec.width = 2560;         \
    spec.height = 1440;

#define RESOLUTION_4K(spec)    \
    spec.width = 3840;         \
    spec.height = 2160;


//Style
/*
* Get first, then setters
*/

int main()
{
	core::WindowSpecification windowSpecification;
	windowSpecification.windowTitle = "OpenGL 4.6";
	windowSpecification.width = 1920;
	windowSpecification.height = 1080;

	//glfwInit();
	//Window window(windowSpecification);
	//window.makeContextCurrent();
	//window.lockMouse();
	//window.setupInputs();
	//window.uncapFPS();
	//glewInit();

	core::Application app = core::Application(windowSpecification);
	app.pushLayer<core::RenderLayer>();
	app.run();

	//glfwInit();
	//Window window(windowSpecification);
	//window.makeContextCurrent();
	//window.lockMouse();
	//window.setupInputs();
	//window.uncapFPS();
	//glewInit();

	Camera camera(0.1, 90.0f, 0.1f, 5000.0f);
	OpenGLRenderer openGLRenderer;
	ShaderProgram shader("shader/VertexShader.glsl", "shader/FragmentShader.glsl");
	shader.bind();


	StaticMesh* worldObject = FileLoader::readObj("objects/backpack.obj", "objects");
	StaticMeshObject backPack(worldObject, 0, 0, 0);
	//backPack.setScale(500.0f);


	PointLightSource pointLight {
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

	SpotLightSource spotLight {
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

	DirectionalLightSource dirL {
		glm::vec3(0.5f, -0.7f, 0.5f),
		// AMBIENT: Extremely low (0.02). This ensures back-faces are nearly black, 
		// but not "void" black (so you can essentially only see the lit side).
		glm::vec3(0.02f, 0.02f, 0.02f),

		// DIFFUSE: Soft Sunlight
		glm::vec3(0.8f, 0.8f, 0.75f),
		// SPECULAR
		glm::vec3(1.0f, 1.0f, 1.0f)
	};

	std::vector<PointLightSource> pointLights = {pointLight};
	std::vector<SpotLightSource> spotLights = {spotLight};
	std::vector<DirectionalLightSource> directionalLights = {};

	openGLRenderer.uploadLightSources(pointLights, shader);
	openGLRenderer.uploadLightSources(spotLights, shader);
	openGLRenderer.uploadLightSources(directionalLights, shader);

	while (!window.shouldClose()) {
		core::input::InputManager::getInstance()->update(); //TEMP
		window.pollEvents();
		window.clearColor();
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);

		camera.update(window, window.getFrameDeltaTime());

		static float x = 0;
		x += 0.01f;
		backPack.setRotation(0.0f, x, 0.0f);
		camera.updateCameraVectors();

		openGLRenderer.render(backPack);
		openGLRenderer.renderScene(shader, camera, window);
		window.swapBuffers(); //used
	}

	return 0;
}