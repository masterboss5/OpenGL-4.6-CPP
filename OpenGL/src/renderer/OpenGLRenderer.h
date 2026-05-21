#pragma once
#include <vector>
#include "src/scene/StaticMeshObject.h"
#include "ShaderProgram.h"
#include "src/scene/DirectionalLightSource.h"
#include "LightBufferManager.h"
#include "src/scene/Camera.h"
#include "src/core/input/Window.h"

class OpenGLRenderer final
{
private:
	GLuint instanceSSBO;
	unsigned int drawCount;
	unsigned int objectsDrawn;
	std::vector<DirectionalLightSource> directionalLightSources;
	LightBufferManager lightBufferManager;
	std::unordered_map<const StaticMesh*, std::vector<StaticMeshObject>> batch;
public:
	OpenGLRenderer();
	OpenGLRenderer(const OpenGLRenderer&) = delete;
	OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;
	OpenGLRenderer(OpenGLRenderer&&) = delete;
	OpenGLRenderer& operator=(OpenGLRenderer&&) = delete;

	template<typename LightType>
	void uploadLightSources(std::vector<LightType>& lightSources, ShaderProgram& shaderProgram)
	{
		this->lightBufferManager.uploadLightSources(lightSources, shaderProgram);
	}

	unsigned int getDrawCount() const;
	unsigned int getObjectsDrawn() const;
	void render(const StaticMeshObject& worldObject);
	void renderScene(const ShaderProgram& shader, const Camera& camera, const Window& window);
	void enableCulling() const;
	void disableCulling() const;
};