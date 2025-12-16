#pragma once
#include <vector>
#include "StaticWorldObject.h"
#include <map>
#include "ShaderProgram.h"
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include "SpotLightSource.h"
#include "Material.h"

class OpenGLRenderer final
{
private:
	inline static unsigned int instanceSSBO;
	inline static unsigned int lightSourcesSSBO;
	unsigned int drawCount;
	unsigned int objectsDrawn;
	std::vector<SpotLightSource> lightSources;
	std::unordered_map<const StaticGeometry*, std::vector<StaticWorldObject>> batch;
public:
	OpenGLRenderer();
	OpenGLRenderer(const OpenGLRenderer&) = delete;
	OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;
	OpenGLRenderer(OpenGLRenderer&&) = delete;
	OpenGLRenderer& operator=(OpenGLRenderer&&) = delete;

	unsigned int getDrawCount() const;
	unsigned int getObjectsDrawn() const;
	void addLightSource(const SpotLightSource& lightSource);
	void uploadLightSources(const ShaderProgram& shader) const;
	void render(const StaticWorldObject& worldObject);
	void renderScene(const ShaderProgram& shader, const glm::mat4& projection);
};