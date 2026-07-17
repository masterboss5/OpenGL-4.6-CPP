#include "LightBufferManager.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "src/scene/SceneCollection.h"

namespace
{
	template<typename LightType>
	[[nodiscard]] float32 calculateInfluenceRange(const LightType& light)
	{
		constexpr float32 minimumContribution = 0.01f;
		const float32 peakIntensity = std::max({ light.diffuse.r, light.diffuse.g, light.diffuse.b, minimumContribution });
		const float32 constantTerm = light.constant - peakIntensity / minimumContribution;
		if (light.quadratic > 0.0f)
		{
			const float32 discriminant = light.linear * light.linear - 4.0f * light.quadratic * constantTerm;
			if (discriminant > 0.0f) return std::max((-light.linear + std::sqrt(discriminant)) / (2.0f * light.quadratic), 0.0f);
		}
		if (light.linear > 0.0f) return std::max(-constantTerm / light.linear, 0.0f);
		return 1000.0f;
	}
}

LightBufferManager::LightBufferManager(size_t maxLights)
	: maxLights(static_cast<uint32>(maxLights)),
	  pointLightSourcesSSBO(maxLights),
	  spotLightSourcesSSBO(maxLights),
	directionalLightSourcesSSBO(maxLights)
{
	glCreateBuffers(1, &this->unifiedLightBuffer);
	glNamedBufferStorage(this->unifiedLightBuffer, static_cast<GLsizeiptr>(sizeof(renderer::GpuLightRecord) * this->maxLights), nullptr, GL_DYNAMIC_STORAGE_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Lights), this->unifiedLightBuffer);
}

LightBufferManager::~LightBufferManager()
{
	if (this->unifiedLightBuffer != 0) glDeleteBuffers(1, &this->unifiedLightBuffer);
}

uint32 LightBufferManager::getTotalLightSourceCount() const
{
	return static_cast<uint32>(this->pointLightSources.size() + this->spotLightSources.size() + this->directionalLightSources.size());
}

const std::vector<PointLightSource>& LightBufferManager::getPointLights() const noexcept { return this->pointLightSources; }
const std::vector<SpotLightSource>& LightBufferManager::getSpotLights() const noexcept { return this->spotLightSources; }
const std::vector<DirectionalLightSource>& LightBufferManager::getDirectionalLights() const noexcept { return this->directionalLightSources; }

void LightBufferManager::clear()
{
	this->pointLightSources.clear();
	this->spotLightSources.clear();
	this->directionalLightSources.clear();
}

void LightBufferManager::uploadLightSources(std::vector<PointLightSource>& lightSources)
{
	this->pointLightSources = lightSources;
	this->pointLightSourcesSSBO.upload(lightSources.data(), lightSources.size());
	this->uploadUnifiedLightBuffer();
}

void LightBufferManager::uploadLightSources(std::vector<SpotLightSource>& lightSources)
{
	this->spotLightSources = lightSources;
	this->spotLightSourcesSSBO.upload(lightSources.data(), lightSources.size());
	this->uploadUnifiedLightBuffer();
}

void LightBufferManager::uploadLightSources(std::vector<DirectionalLightSource>& lightSources)
{
	this->directionalLightSources = lightSources;
	this->directionalLightSourcesSSBO.upload(lightSources.data(), lightSources.size());
	this->uploadUnifiedLightBuffer();
}

void LightBufferManager::uploadSceneLights(const SceneCollection& scene)
{
	this->directionalLightSources = scene.getDirectionalLights();
	this->pointLightSources = scene.getPointLights();
	this->spotLightSources = scene.getSpotLights();
	this->uploadUnifiedLightBuffer();
}

void LightBufferManager::uploadUnifiedLightBuffer()
{
	std::vector<renderer::GpuLightRecord> gpuLights;
	gpuLights.reserve(this->directionalLightSources.size() + this->pointLightSources.size() + this->spotLightSources.size());
	for (uint32 lightIndex = 0; lightIndex < this->directionalLightSources.size(); ++lightIndex) { const DirectionalLightSource& light = this->directionalLightSources[lightIndex]; gpuLights.push_back({ .positionAndRange = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), .directionAndType = glm::vec4(glm::normalize(light.direction), 0.0f), .colorAndIntensity = glm::vec4(light.diffuse, 1.0f), .spotAnglesAndShadow = glm::vec4(0.0f, 0.0f, 0.0f, lightIndex == 0 ? 0.0f : -1.0f) }); }
	for (uint32 lightIndex = 0; lightIndex < this->pointLightSources.size(); ++lightIndex) { const PointLightSource& light = this->pointLightSources[lightIndex]; gpuLights.push_back({ .positionAndRange = glm::vec4(light.position, calculateInfluenceRange(light)), .directionAndType = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), .colorAndIntensity = glm::vec4(light.diffuse, 1.0f), .spotAnglesAndShadow = glm::vec4(0.0f, 0.0f, 0.0f, static_cast<float32>(lightIndex)) }); }
	for (uint32 lightIndex = 0; lightIndex < this->spotLightSources.size(); ++lightIndex) { const SpotLightSource& light = this->spotLightSources[lightIndex]; gpuLights.push_back({ .positionAndRange = glm::vec4(light.position, calculateInfluenceRange(light)), .directionAndType = glm::vec4(glm::normalize(light.direction), 2.0f), .colorAndIntensity = glm::vec4(light.diffuse, 1.0f), .spotAnglesAndShadow = glm::vec4(light.cutOff, light.outerCutOff, 0.0f, static_cast<float32>(lightIndex)) }); }
	if (gpuLights.size() > this->maxLights) throw std::runtime_error("Unified GPU light buffer capacity exceeded");
	if (!gpuLights.empty()) glNamedBufferSubData(this->unifiedLightBuffer, 0, static_cast<GLsizeiptr>(gpuLights.size() * sizeof(renderer::GpuLightRecord)), gpuLights.data());
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Lights), this->unifiedLightBuffer);
}
