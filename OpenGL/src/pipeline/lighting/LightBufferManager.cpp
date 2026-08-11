#include "src/pipeline/lighting/LightBufferManager.h"

#include "src/concepts.h"
#include "src/scene/SceneCollection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
[[nodiscard]] bool IsFinite(const glm::vec3 &Value) noexcept
{
	return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

void ValidateColor(const glm::vec3 &Value, const string_view Label)
{
	if (!IsFinite(Value) || glm::any(glm::lessThan(Value, glm::vec3(0.0f))))
		throw std::invalid_argument(string(Label) + " must contain finite, non-negative components");
}

void ValidateShadowFlag(const float32 Value)
{
	if (!std::isfinite(Value) || (Value != 0.0f && Value != 1.0f))
		throw std::invalid_argument("Light shadow flag must be exactly zero or one");
}

template <IsAttenuatedLightSource LightType> void ValidateAttenuatedLight(const LightType &Light)
{
	if (!IsFinite(Light.Position))
		throw std::invalid_argument("Attenuated light position must be finite");
	ValidateColor(Light.Ambient, "Light ambient color");
	ValidateColor(Light.Diffuse, "Light diffuse color");
	ValidateColor(Light.Specular, "Light specular color");
	ValidateShadowFlag(Light.CastShadows);
	if (!std::isfinite(Light.Constant) || !std::isfinite(Light.Linear) || !std::isfinite(Light.Quadratic) || Light.Constant < 0.0f ||
		Light.Linear < 0.0f || Light.Quadratic < 0.0f || (Light.Constant == 0.0f && Light.Linear == 0.0f && Light.Quadratic == 0.0f))
	{
		throw std::invalid_argument("Light attenuation must be finite, non-negative, and contain a non-zero term");
	}
}

template <IsAttenuatedLightSource LightType> [[nodiscard]] float32 CalculateValidatedInfluenceRange(const LightType &Light)
{
	ValidateAttenuatedLight(Light);
	constexpr float32 MinimumContribution = 0.01f;
	const float32 PeakIntensity = std::max({Light.Diffuse.r, Light.Diffuse.g, Light.Diffuse.b, MinimumContribution});
	const float32 ConstantTerm = Light.Constant - PeakIntensity / MinimumContribution;
	if (Light.Quadratic > 0.0f)
	{
		const float32 Discriminant = Light.Linear * Light.Linear - 4.0f * Light.Quadratic * ConstantTerm;
		if (Discriminant > 0.0f)
		{
			const float32 Range = std::max((-Light.Linear + std::sqrt(Discriminant)) / (2.0f * Light.Quadratic), 0.0f);
			if (std::isfinite(Range) && Range > 0.0f)
				return Range;
		}
	}
	if (Light.Linear > 0.0f)
	{
		const float32 Range = std::max(-ConstantTerm / Light.Linear, 0.0f);
		if (std::isfinite(Range) && Range > 0.0f)
			return Range;
	}
	return 1000.0f;
}
} // namespace

namespace pipeline::lighting
{
LightBufferManager::LightBufferManager(const usize MaxLights) : MaxLights(static_cast<uint32>(MaxLights))
{
	if (MaxLights == 0 || MaxLights > std::numeric_limits<uint32>::max())
		throw std::invalid_argument("LightBufferManager capacity must fit a non-zero GPU light count");
	this->GPURecords.reserve(this->MaxLights);
	this->ProposedPointLightSources.reserve(this->MaxLights);
	this->ProposedSpotLightSources.reserve(this->MaxLights);
	this->ProposedDirectionalLightSources.reserve(this->MaxLights);
	this->ProposedGPURecords.reserve(this->MaxLights);
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

std::span<const pipeline::render::GPULightRecord> LightBufferManager::GetGPURecords() const noexcept
{
	return this->GPURecords;
}

float32 LightBufferManager::CalculateInfluenceRange(const PointLightSource &Light)
{
	return CalculateValidatedInfluenceRange(Light);
}

float32 LightBufferManager::CalculateInfluenceRange(const SpotLightSource &Light)
{
	return CalculateValidatedInfluenceRange(Light);
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
	this->ProposedPointLightSources.assign(LightSources.begin(), LightSources.end());
	this->BuildGPURecordsInto(this->ProposedPointLightSources, this->SpotLightSources, this->DirectionalLightSources,
							  this->ProposedGPURecords);
	this->PointLightSources.swap(this->ProposedPointLightSources);
	this->GPURecords.swap(this->ProposedGPURecords);
}

void LightBufferManager::UploadLightSources(const std::vector<SpotLightSource> &LightSources)
{
	this->ProposedSpotLightSources.assign(LightSources.begin(), LightSources.end());
	this->BuildGPURecordsInto(this->PointLightSources, this->ProposedSpotLightSources, this->DirectionalLightSources,
							  this->ProposedGPURecords);
	this->SpotLightSources.swap(this->ProposedSpotLightSources);
	this->GPURecords.swap(this->ProposedGPURecords);
}

void LightBufferManager::UploadLightSources(const std::vector<DirectionalLightSource> &LightSources)
{
	this->ProposedDirectionalLightSources.assign(LightSources.begin(), LightSources.end());
	this->BuildGPURecordsInto(this->PointLightSources, this->SpotLightSources, this->ProposedDirectionalLightSources,
							  this->ProposedGPURecords);
	this->DirectionalLightSources.swap(this->ProposedDirectionalLightSources);
	this->GPURecords.swap(this->ProposedGPURecords);
}

void LightBufferManager::UploadSceneLights(const SceneCollection &Scene)
{
	const std::vector<DirectionalLightSource> &Directional = Scene.GetDirectionalLights();
	const std::vector<PointLightSource> &Points = Scene.GetPointLights();
	const std::vector<SpotLightSource> &Spots = Scene.GetSpotLights();
	this->ProposedDirectionalLightSources.assign(Directional.begin(), Directional.end());
	this->ProposedPointLightSources.assign(Points.begin(), Points.end());
	this->ProposedSpotLightSources.assign(Spots.begin(), Spots.end());
	this->BuildGPURecordsInto(this->ProposedPointLightSources, this->ProposedSpotLightSources, this->ProposedDirectionalLightSources,
							  this->ProposedGPURecords);
	this->DirectionalLightSources.swap(this->ProposedDirectionalLightSources);
	this->PointLightSources.swap(this->ProposedPointLightSources);
	this->SpotLightSources.swap(this->ProposedSpotLightSources);
	this->GPURecords.swap(this->ProposedGPURecords);
}

std::vector<pipeline::render::GPULightRecord> LightBufferManager::BuildGPURecords(
	const std::vector<PointLightSource> &PointLights, const std::vector<SpotLightSource> &SpotLights,
	const std::vector<DirectionalLightSource> &DirectionalLights) const
{
	std::vector<pipeline::render::GPULightRecord> Records;
	this->BuildGPURecordsInto(PointLights, SpotLights, DirectionalLights, Records);
	return Records;
}

void LightBufferManager::BuildGPURecordsInto(const std::vector<PointLightSource> &PointLights,
											 const std::vector<SpotLightSource> &SpotLights,
											 const std::vector<DirectionalLightSource> &DirectionalLights,
											 std::vector<pipeline::render::GPULightRecord> &Records) const
{
	if (DirectionalLights.size() > this->MaxLights || PointLights.size() > this->MaxLights || SpotLights.size() > this->MaxLights ||
		DirectionalLights.size() + PointLights.size() > this->MaxLights ||
		DirectionalLights.size() + PointLights.size() + SpotLights.size() > this->MaxLights)
	{
		throw std::runtime_error("Unified GPU light buffer capacity exceeded");
	}
	Records.clear();
	Records.reserve(DirectionalLights.size() + PointLights.size() + SpotLights.size());
	bool DirectionalShadowAllocated = false;
	for (const DirectionalLightSource &Light : DirectionalLights)
	{
		if (!IsFinite(Light.Direction) || glm::dot(Light.Direction, Light.Direction) <= std::numeric_limits<float32>::epsilon())
			throw std::invalid_argument("Directional light direction must be finite and non-zero");
		ValidateColor(Light.Ambient, "Directional light ambient color");
		ValidateColor(Light.Diffuse, "Directional light diffuse color");
		ValidateColor(Light.Specular, "Directional light specular color");
		ValidateShadowFlag(Light.CastShadows);
		const bool AllocateShadow = Light.CastShadows > 0.5f && !DirectionalShadowAllocated;
		DirectionalShadowAllocated = DirectionalShadowAllocated || AllocateShadow;
		Records.push_back({.PositionAndRange = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
						   .DirectionAndType = glm::vec4(glm::normalize(Light.Direction), 0.0f),
						   .ColorAndIntensity = glm::vec4(Light.Diffuse, 1.0f),
						   .SpotAnglesAndShadow = glm::vec4(0.0f, 0.0f, 0.0f, AllocateShadow ? 0.0f : -1.0f)});
	}
	uint32 PointShadowIndex = 0;
	for (uint32 LightIndex = 0; LightIndex < PointLights.size(); ++LightIndex)
	{
		const PointLightSource &Light = PointLights[LightIndex];
		const float32 InfluenceRange = CalculateValidatedInfluenceRange(Light);
		const bool AllocateShadow = Light.CastShadows > 0.5f && PointShadowIndex < pipeline::render::MaximumPointShadowCount;
		const float32 ShadowIndex = AllocateShadow ? static_cast<float32>(PointShadowIndex++) : -1.0f;
		Records.push_back({.PositionAndRange = glm::vec4(Light.Position, InfluenceRange),
						   .DirectionAndType = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
						   .ColorAndIntensity = glm::vec4(Light.Diffuse, 1.0f),
						   .SpotAnglesAndShadow = glm::vec4(0.0f, 0.0f, 0.0f, ShadowIndex)});
	}
	uint32 SpotShadowIndex = 0;
	for (uint32 LightIndex = 0; LightIndex < SpotLights.size(); ++LightIndex)
	{
		const SpotLightSource &Light = SpotLights[LightIndex];
		const float32 InfluenceRange = CalculateValidatedInfluenceRange(Light);
		if (!IsFinite(Light.Direction) || glm::dot(Light.Direction, Light.Direction) <= std::numeric_limits<float32>::epsilon())
			throw std::invalid_argument("Spot light direction must be finite and non-zero");
		if (!std::isfinite(Light.CutOff) || !std::isfinite(Light.OuterCutOff) || Light.OuterCutOff <= 0.0f || Light.CutOff > 1.0f ||
			Light.CutOff <= Light.OuterCutOff)
		{
			throw std::invalid_argument("Spot light cone cosines must satisfy zero < outer < inner <= one");
		}
		const bool AllocateShadow = Light.CastShadows > 0.5f && SpotShadowIndex < pipeline::render::MaximumSpotShadowCount;
		const float32 ShadowIndex = AllocateShadow ? static_cast<float32>(SpotShadowIndex++) : -1.0f;
		Records.push_back({.PositionAndRange = glm::vec4(Light.Position, InfluenceRange),
						   .DirectionAndType = glm::vec4(glm::normalize(Light.Direction), 2.0f),
						   .ColorAndIntensity = glm::vec4(Light.Diffuse, 1.0f),
						   .SpotAnglesAndShadow = glm::vec4(Light.CutOff, Light.OuterCutOff, 0.0f, ShadowIndex)});
	}
}
} // namespace pipeline::lighting
