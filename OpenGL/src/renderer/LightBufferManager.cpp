#include "LightBufferManager.h"

#include "src/concepts.h"
#include "src/scene/SceneCollection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
template <IsAttenuatedLightSource LightType> [[nodiscard]] float32 CalculateInfluenceRange(const LightType &Light)
{
	constexpr float32 MinimumContribution = 0.01f;
	const float32 PeakIntensity = std::max({Light.Diffuse.r, Light.Diffuse.g, Light.Diffuse.b, MinimumContribution});
	const float32 ConstantTerm = Light.Constant - PeakIntensity / MinimumContribution;
	if (Light.Quadratic > 0.0f)
	{
		const float32 Discriminant = Light.Linear * Light.Linear - 4.0f * Light.Quadratic * ConstantTerm;
		if (Discriminant > 0.0f)
			return std::max((-Light.Linear + std::sqrt(Discriminant)) / (2.0f * Light.Quadratic), 0.0f);
	}
	if (Light.Linear > 0.0f)
		return std::max(-ConstantTerm / Light.Linear, 0.0f);
	return 1000.0f;
}
} // namespace

LightBufferManager::LightBufferManager(const usize MaxLights) : MaxLights(static_cast<uint32>(MaxLights))
{
	if (MaxLights == 0 || MaxLights > std::numeric_limits<uint32>::max())
		throw std::invalid_argument("LightBufferManager capacity must fit a non-zero GPU light count");
	this->GPURecords.reserve(this->MaxLights);
}

uint32 LightBufferManager::GetTotalLightSourceCount() const
{
	return static_cast<uint32>(this->PointLightSources.size() + this->SpotLightSources.size() + this->DirectionalLightSources.size());
}

const std::vector<PointLightSource> &LightBufferManager::GetPointLights() const noexcept
{
	return this->PointLightSources;
}
const std::vector<SpotLightSource> &LightBufferManager::GetSpotLights() const noexcept
{
	return this->SpotLightSources;
}
const std::vector<DirectionalLightSource> &LightBufferManager::GetDirectionalLights() const noexcept
{
	return this->DirectionalLightSources;
}

std::span<const renderer::GPULightRecord> LightBufferManager::GetGPURecords() const noexcept
{
	return this->GPURecords;
}

void LightBufferManager::Clear()
{
	this->PointLightSources.clear();
	this->SpotLightSources.clear();
	this->DirectionalLightSources.clear();
	this->GPURecords.clear();
}

void LightBufferManager::UploadLightSources(const std::vector<PointLightSource> &LightSources)
{
	std::vector<PointLightSource> Proposed = LightSources;
	auto Records = this->BuildGPURecords(Proposed, this->SpotLightSources, this->DirectionalLightSources);
	this->PointLightSources = std::move(Proposed);
	this->GPURecords = std::move(Records);
}

void LightBufferManager::UploadLightSources(const std::vector<SpotLightSource> &LightSources)
{
	std::vector<SpotLightSource> Proposed = LightSources;
	auto Records = this->BuildGPURecords(this->PointLightSources, Proposed, this->DirectionalLightSources);
	this->SpotLightSources = std::move(Proposed);
	this->GPURecords = std::move(Records);
}

void LightBufferManager::UploadLightSources(const std::vector<DirectionalLightSource> &LightSources)
{
	std::vector<DirectionalLightSource> Proposed = LightSources;
	auto Records = this->BuildGPURecords(this->PointLightSources, this->SpotLightSources, Proposed);
	this->DirectionalLightSources = std::move(Proposed);
	this->GPURecords = std::move(Records);
}

void LightBufferManager::UploadSceneLights(const SceneCollection &Scene)
{
	std::vector<DirectionalLightSource> Directional = Scene.GetDirectionalLights();
	std::vector<PointLightSource> Points = Scene.GetPointLights();
	std::vector<SpotLightSource> Spots = Scene.GetSpotLights();
	auto Records = this->BuildGPURecords(Points, Spots, Directional);
	this->DirectionalLightSources = std::move(Directional);
	this->PointLightSources = std::move(Points);
	this->SpotLightSources = std::move(Spots);
	this->GPURecords = std::move(Records);
}

std::vector<renderer::GPULightRecord> LightBufferManager::BuildGPURecords(
	const std::vector<PointLightSource> &PointLights, const std::vector<SpotLightSource> &SpotLights,
	const std::vector<DirectionalLightSource> &DirectionalLights) const
{
	if (DirectionalLights.size() > this->MaxLights || PointLights.size() > this->MaxLights || SpotLights.size() > this->MaxLights ||
		DirectionalLights.size() + PointLights.size() > this->MaxLights ||
		DirectionalLights.size() + PointLights.size() + SpotLights.size() > this->MaxLights)
	{
		throw std::runtime_error("Unified GPU light buffer capacity exceeded");
	}
	std::vector<renderer::GPULightRecord> Records;
	Records.reserve(DirectionalLights.size() + PointLights.size() + SpotLights.size());
	for (uint32 LightIndex = 0; LightIndex < DirectionalLights.size(); ++LightIndex)
	{
		const DirectionalLightSource &Light = DirectionalLights[LightIndex];
		Records.push_back({.PositionAndRange = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
						   .DirectionAndType = glm::vec4(glm::normalize(Light.Direction), 0.0f),
						   .ColorAndIntensity = glm::vec4(Light.Diffuse, 1.0f),
						   .SpotAnglesAndShadow = glm::vec4(0.0f, 0.0f, 0.0f, LightIndex == 0 ? 0.0f : -1.0f)});
	}
	for (uint32 LightIndex = 0; LightIndex < PointLights.size(); ++LightIndex)
	{
		const PointLightSource &Light = PointLights[LightIndex];
		const float32 ShadowIndex = LightIndex < renderer::MaximumPointShadowCount ? static_cast<float32>(LightIndex) : -1.0f;
		Records.push_back({.PositionAndRange = glm::vec4(Light.Position, CalculateInfluenceRange(Light)),
						   .DirectionAndType = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
						   .ColorAndIntensity = glm::vec4(Light.Diffuse, 1.0f),
						   .SpotAnglesAndShadow = glm::vec4(0.0f, 0.0f, 0.0f, ShadowIndex)});
	}
	for (uint32 LightIndex = 0; LightIndex < SpotLights.size(); ++LightIndex)
	{
		const SpotLightSource &Light = SpotLights[LightIndex];
		const float32 ShadowIndex = LightIndex < renderer::MaximumSpotShadowCount ? static_cast<float32>(LightIndex) : -1.0f;
		Records.push_back({.PositionAndRange = glm::vec4(Light.Position, CalculateInfluenceRange(Light)),
						   .DirectionAndType = glm::vec4(glm::normalize(Light.Direction), 2.0f),
						   .ColorAndIntensity = glm::vec4(Light.Diffuse, 1.0f),
						   .SpotAnglesAndShadow = glm::vec4(Light.CutOff, Light.OuterCutOff, 0.0f, ShadowIndex)});
	}
	return Records;
}
