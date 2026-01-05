#pragma once
#include <vector>
#include "StaticWorldObject.h"
#include <map>
#include "ShaderProgram.h"
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include "SpotLightSource.h"
#include "DirectionalLightSource.h"
#include "PointLightSource.h"
#include "Material.h"
#include "LightBufferManager.h"

class OpenGLRenderer final
{
private:
	GLuint instanceSSBO;
	GLuint lightSourcesSSBO;
	unsigned int drawCount;
	unsigned int objectsDrawn;
	std::vector<DirectionalLightSource> directionalLightSources;
	std::vector<SpotLightSource> spotLightSources;
	std::vector<PointLightSource> pointLightSources;
	std::unordered_map<const StaticGeometry*, std::vector<StaticWorldObject>> batch;
public:
	OpenGLRenderer();
	OpenGLRenderer(const OpenGLRenderer&) = delete;
	OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;
	OpenGLRenderer(OpenGLRenderer&&) = delete;
	OpenGLRenderer& operator=(OpenGLRenderer&&) = delete;

	unsigned int getDrawCount() const;
	unsigned int getObjectsDrawn() const;
	void addLightSource(const DirectionalLightSource& lightSource);
	void addLightSource(const SpotLightSource& lightSource);
	void addLightSource(const PointLightSource& lightSource);
	void uploadLightSources(const ShaderProgram& shader) const;
	void render(const StaticWorldObject& worldObject);
	void renderScene(const ShaderProgram& shader, const glm::mat4& projection);
};