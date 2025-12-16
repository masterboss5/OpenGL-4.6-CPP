#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Window.h"
#include "StaticGeometry.h"
#include <vector>
#include "ShaderProgram.h"
#include <iostream>
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include "OpenGLRenderer.h"
#include "Texture.h"
#include "FileLoader.cpp"
#include "SpotLightSource.h"
#include "Camera.h"

#define FAR_PLANE 600.0f
#define NEAR_PLANE 0.1f

//Style
/*
* Get first, then setters
*/

int main()
{
	glfwInit();
	Window window("OpenGL 4.6", 2560, 1440);
	window.makeContextCurrent();
	window.lockMouse();
	window.setupInputs();
	window.uncapFPS();
	glewInit();

	Camera cam(0.1, 90.0f, 0.1f, 5000.0f);
	OpenGLRenderer openGLRenderer;
	ShaderProgram shader("shader/VertexShader.glsl", "shader/FragmentShader.glsl");
	shader.bind();
	Texture texture("image/img.png");

	window.registerKeyPressEvent(GLFW_KEY_ESCAPE, [&](int key) {
		window.closeWindow();
	});

	SpotLightSource pointLight(
		glm::vec3(0.0f, 0.0f, -10.0f),
		glm::vec3(0.0f, 0.0f, 0.0f),
		glm::cos(glm::radians(12.5f)),
		glm::cos(glm::radians(17.5f)),
		glm::vec3(0.1f, 0.1f, 0.1f),
		glm::vec3(0.8f, 0.8f, 0.8f),
		glm::vec3(1.0f, 1.0f, 1.0f),
		1.0f,
		0.001f,
		0.00001f
	);

	pointLight.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
	
	StaticGeometry* worldObject = FileLoader::readObj("objects/backpack.obj", "objects");
	StaticWorldObject backPack(worldObject, 0, 0, 0);
	//backPack.setScale(500.0f);

	culling: {
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		glFrontFace(GL_CCW);
	}

	float x = 0.0f;
	openGLRenderer.addLightSource(pointLight);
	while (!window.shouldClose()) {
		window.pollEvents();
		window.clearColor();
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);

		cam.tick(window, window.getFrameDeltaTime());

		glm::mat4 projectionMatrix = cam.getProjectionMatrix(window);

		x += 0.01f;
		backPack.setRotation(0.0f, x, 0.0f);
		cam.updateCameraVectors();

		glm::mat4 camView = cam.getViewMatrix();
		glUniformMatrix4fv(shader.getUniformLocation("projection"), 1, 0, glm::value_ptr(projectionMatrix));
		glUniformMatrix4fv(shader.getUniformLocation("view"), 1, 0, glm::value_ptr(camView));
		glUniform3f(shader.getUniformLocation("viewPos"), cam.position.x, cam.position.y, cam.position.z);

		openGLRenderer.render(backPack);
		openGLRenderer.renderScene(shader, projectionMatrix);
		window.swapBuffers();
	}

	return 0;
}
