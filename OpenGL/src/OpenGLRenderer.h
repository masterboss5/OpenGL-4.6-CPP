#pragma once
#include <vector>
#include "StaticMeshObject.h"
#include <map>
#include "ShaderProgram.h"
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include "SpotLightSource.h"
#include "DirectionalLightSource.h"
#include "PointLightSource.h"
#include "Material.h"
#include "LightBufferManager.h"
#include "Camera.h"
#include "Window.h"

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