#include "LightBufferManager.h"

LightBufferManager::LightBufferManager(size_t maxLights)
	: pointLightSourcesSSBO(maxLights),
	  spotLightSourcesSSBO(maxLights),
	directionalLightSourcesSSBO(maxLights)
{

}

int LightBufferManager::getTotalLightSourceCount() const
{
	return this->pointLightSources.size() + this->spotLightSources.size() + this->directionalLightSources.size();
}

void LightBufferManager::clear()
{
	this->pointLightSources.clear();
	this->spotLightSources.clear();
	this->directionalLightSources.clear();
}

void LightBufferManager::uploadLightSources(std::vector<PointLightSource>& lightSources, ShaderProgram& shaderProgram)
{
	this->pointLightSourcesSSBO.upload(lightSources.data(), lightSources.size(), shaderProgram);
	glUniform1i(shaderProgram.getUniformLocation("pointLightSourcesCount"), lightSources.size());
}

void LightBufferManager::uploadLightSources(std::vector<SpotLightSource>& lightSources, ShaderProgram& shaderProgram)
{
	this->spotLightSourcesSSBO.upload(lightSources.data(), lightSources.size(), shaderProgram);
	glUniform1i(shaderProgram.getUniformLocation("spotLightSourcesCount"), lightSources.size());
}

void LightBufferManager::uploadLightSources(std::vector<DirectionalLightSource>& lightSources, ShaderProgram& shaderProgram)
{
	this->directionalLightSourcesSSBO.upload(lightSources.data(), lightSources.size(), shaderProgram);
	glUniform1i(shaderProgram.getUniformLocation("directionalLightSourcesCount"), lightSources.size());
}