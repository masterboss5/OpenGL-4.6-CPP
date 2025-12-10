#include <GL/glew.h>
#include "OpenGLRenderer.h"

#define MAX_INSTANCES_PER_BATCH 3250
#define MAX_LIGHT_SOURCES 100
#define __DEBUG_MAP_BUFFER

OpenGLRenderer::OpenGLRenderer() : drawCount(0), objectsDrawn(0)
{
	glGenBuffers(1, &instanceSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, instanceSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(glm::mat4) * MAX_INSTANCES_PER_BATCH, nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, instanceSSBO);

	glGenBuffers(1, &lightSourcesSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSourcesSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(LightSource) * MAX_LIGHT_SOURCES, nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, lightSourcesSSBO);
}

unsigned int OpenGLRenderer::getDrawCount() const 
{
	return this->drawCount;
}

unsigned int OpenGLRenderer::getObjectsDrawn() const
{
	return this->objectsDrawn;
}

void OpenGLRenderer::addLightSource(const LightSource& lightSource)
{
	this->lightSources.push_back(lightSource);
}

void OpenGLRenderer::uploadLightSources(const ShaderProgram& shader) const
{
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSourcesSSBO);
	void* const lightSSBO = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(LightSource) * this->lightSources.size(), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
	std::memcpy(lightSSBO, this->lightSources.data(), sizeof(LightSource) * this->lightSources.size());
	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
	glUniform1i(shader.getUniformLocation("lightCount"), this->lightSources.size());
}

void OpenGLRenderer::render(const StaticWorldObject& worldObject)
{
	this->batch[worldObject.getGameObject()].push_back(worldObject);
	this->objectsDrawn++;
}

void OpenGLRenderer::renderScene(const ShaderProgram& shader, const glm::mat4& projection)
{
	shader.bind();
	this->uploadLightSources(shader);

	for (const auto& [staticGeometry, objects] : this->batch) {
		glBindVertexArray(staticGeometry->getVAO());

		std::vector<glm::mat4> transforms;
		transforms.reserve(objects.size());
		for (const auto& object : objects) {
			glm::mat4 model = glm::mat4(1.0f);
			object.buildMatrix(model);
			transforms.push_back(model);
		}

		GLsizeiptr size = sizeof(glm::mat4) * objects.size();
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, instanceSSBO);

#ifdef __DEBUG_MAP_BUFFER
		void* const transformSSBO = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(glm::mat4) * transforms.size(), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
		std::memcpy(transformSSBO, transforms.data(), sizeof(glm::mat4) * transforms.size());
		glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
#else
		glBufferData(GL_SHADER_STORAGE_BUFFER, size, transforms.data(), GL_DYNAMIC_DRAW);
#endif

		staticGeometry->getMaterial().applyToShader(shader);

		glDrawElementsInstanced(GL_TRIANGLES, staticGeometry->getIndicesCount(), GL_UNSIGNED_INT, nullptr, objects.size());
		this->drawCount++;
	}

	//std::cout << " Draw Calls: " << this->drawCount << " | Objects Drawn: " << this->objectsDrawn << std::endl;

	this->batch.clear();
	this->drawCount = 0;
	//this->lightSources.clear();
	this->objectsDrawn = 0;
}