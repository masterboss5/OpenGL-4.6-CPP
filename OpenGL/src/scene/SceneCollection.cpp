#include "SceneCollection.h"

#include <limits>
#include <stdexcept>

void SceneCollection::BeginFrame(uint64 NewFrameNumber)
{
	this->Clear();
	this->FrameNumber = NewFrameNumber;
}

void SceneCollection::Submit(renderer::RenderItem Item)
{
	if (this->Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (Item.VertexArray == 0 || Item.VertexDescriptor == nullptr || Item.IndexCount == 0 || !Item.MaterialGeneration)
	{
		throw std::invalid_argument(
			"RenderItem requires realized geometry, a vertex descriptor, an index range, and a material generation");
	}
	this->RenderItems.push_back(std::move(Item));
}

uint32 SceneCollection::AppendSkinningPalette(std::span<const glm::mat4> Current, std::span<const glm::mat4> Previous)
{
	if (this->Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (Current.empty() || Current.size() != Previous.size())
		throw std::invalid_argument("Skinning palettes require equal non-empty current and previous poses");
	if (this->SkinningMatrices.size() > std::numeric_limits<uint32>::max() - Current.size())
		throw std::overflow_error("Skinning palette offset exceeds the GPU record range");
	const uint32 Offset = static_cast<uint32>(this->SkinningMatrices.size());
	for (usize Index = 0; Index < Current.size(); ++Index)
		this->SkinningMatrices.push_back({Current[Index], Previous[Index]});
	return Offset;
}

uint32 SceneCollection::AppendMorphWeights(std::span<const renderer::GPUMorphWeightRecord> Weights)
{
	if (this->Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (Weights.empty())
		throw std::invalid_argument("Morph weight publication requires at least one active target");
	if (this->MorphWeights.size() > std::numeric_limits<uint32>::max() - Weights.size())
		throw std::overflow_error("Morph weight offset exceeds the GPU record range");
	const uint32 Offset = static_cast<uint32>(this->MorphWeights.size());
	this->MorphWeights.insert(this->MorphWeights.end(), Weights.begin(), Weights.end());
	return Offset;
}

void SceneCollection::AddDirectionalLight(const DirectionalLightSource &Light)
{
	if (Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	DirectionalLights.push_back(Light);
}
void SceneCollection::AddPointLight(const PointLightSource &Light)
{
	if (Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	PointLights.push_back(Light);
}
void SceneCollection::AddSpotLight(const SpotLightSource &Light)
{
	if (Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	SpotLights.push_back(Light);
}
void SceneCollection::Seal()
{
	this->Sealed = true;
}
void SceneCollection::Clear()
{
	this->Sealed = false;
	RenderItems.clear();
	DirectionalLights.clear();
	PointLights.clear();
	SpotLights.clear();
	SkinningMatrices.clear();
	MorphWeights.clear();
	AssetPins.clear();
}

std::vector<resource::AssetPtr<resource::Asset>> SceneCollection::ReleaseAssetPins() noexcept
{
	std::vector<resource::AssetPtr<resource::Asset>> Pins;
	Pins.swap(this->AssetPins);
	return Pins;
}
uint64 SceneCollection::GetFrameNumber() const noexcept
{
	return FrameNumber;
}
bool SceneCollection::IsSealed() const noexcept
{
	return Sealed;
}
const std::vector<renderer::RenderItem> &SceneCollection::GetRenderItems() const noexcept
{
	return RenderItems;
}
const std::vector<DirectionalLightSource> &SceneCollection::GetDirectionalLights() const noexcept
{
	return DirectionalLights;
}
const std::vector<PointLightSource> &SceneCollection::GetPointLights() const noexcept
{
	return PointLights;
}
const std::vector<SpotLightSource> &SceneCollection::GetSpotLights() const noexcept
{
	return SpotLights;
}
const std::vector<renderer::GPUSkinMatrixRecord> &SceneCollection::GetSkinningMatrices() const noexcept
{
	return SkinningMatrices;
}
const std::vector<renderer::GPUMorphWeightRecord> &SceneCollection::GetMorphWeights() const noexcept
{
	return MorphWeights;
}
