#include "SceneCollection.h"

#include <limits>
#include <stdexcept>

namespace
{
constexpr uint32 PickTableRingSize = 3;
}

SceneCollection::SceneCollection(const SceneCollectionCapacitySpecification Specification) : Capacity(Specification)
{
	if (Specification.RenderItems == 0 || Specification.SkinningMatrices == 0 || Specification.MorphWeights == 0 ||
		Specification.PickObjects == 0 || Specification.DirectionalLights == 0 || Specification.PointLights == 0 ||
		Specification.SpotLights == 0)
		throw std::invalid_argument("Scene collection capacities must be non-zero");
	this->RenderItems.reserve(Specification.RenderItems);
	this->AssetPins.reserve(Specification.RenderItems);
	this->SkinningMatrices.reserve(Specification.SkinningMatrices);
	this->MorphWeights.reserve(Specification.MorphWeights);
	this->DirectionalLights.reserve(Specification.DirectionalLights);
	this->PointLights.reserve(Specification.PointLights);
	this->SpotLights.reserve(Specification.SpotLights);
	this->PickTablePool.reserve(PickTableRingSize);
	for (uint32 Index = 0; Index < PickTableRingSize; ++Index)
		this->PickTablePool.emplace_back(std::make_shared<pipeline::render::FramePickTable>(0, Specification.PickObjects));
}

void SceneCollection::BeginFrame(uint64 NewFrameNumber)
{
	this->Clear();
	this->FrameNumber = NewFrameNumber;
	for (const std::shared_ptr<pipeline::render::FramePickTable> &Candidate : this->PickTablePool)
	{
		if (Candidate.use_count() != 1)
			continue;
		Candidate->Reset(NewFrameNumber);
		this->PickTable = Candidate;
		return;
	}
	throw std::overflow_error("Scene collection pick-table ring is exhausted by in-flight frame consumers");
}

pipeline::render::PickID SceneCollection::RegisterPickObject(const world::ObjectHandle Object)
{
	if (this->Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (this->PickTable == nullptr)
		throw std::logic_error("SceneCollection must begin a frame before registering pickable objects");
	return this->PickTable->Register(Object);
}

void SceneCollection::Submit(pipeline::render::RenderItem Item)
{
	if (this->Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (this->PickTable == nullptr)
		throw std::logic_error("Scene collection must begin a frame before submitting render items");
	if (Item.VertexArray == 0 || Item.VertexDescriptor == nullptr || Item.IndexCount == 0 || !Item.MaterialGeneration)
	{
		throw std::invalid_argument(
			"RenderItem requires realized geometry, a vertex descriptor, an index range, and a material generation");
	}
	if (this->RenderItems.size() >= this->Capacity.RenderItems)
		throw std::overflow_error("Scene collection render-item capacity exceeded");
	this->RenderItems.push_back(std::move(Item));
}

uint32 SceneCollection::AppendSkinningPalette(std::span<const glm::mat4> Current, std::span<const glm::mat4> Previous)
{
	if (this->Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (this->PickTable == nullptr)
		throw std::logic_error("Scene collection must begin a frame before appending skinning data");
	if (Current.empty() || Current.size() != Previous.size())
		throw std::invalid_argument("Skinning palettes require equal non-empty current and previous poses");
	if (this->SkinningMatrices.size() > this->Capacity.SkinningMatrices ||
		Current.size() > this->Capacity.SkinningMatrices - this->SkinningMatrices.size())
		throw std::overflow_error("Scene collection skinning-matrix capacity exceeded");
	if (this->SkinningMatrices.size() > std::numeric_limits<uint32>::max() - Current.size())
		throw std::overflow_error("Skinning palette offset exceeds the GPU record range");
	const uint32 Offset = static_cast<uint32>(this->SkinningMatrices.size());
	for (usize Index = 0; Index < Current.size(); ++Index)
		this->SkinningMatrices.push_back({Current[Index], Previous[Index]});
	return Offset;
}

uint32 SceneCollection::AppendMorphWeights(std::span<const pipeline::render::GPUMorphWeightRecord> Weights)
{
	if (this->Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (this->PickTable == nullptr)
		throw std::logic_error("Scene collection must begin a frame before appending morph data");
	if (Weights.empty())
		throw std::invalid_argument("Morph weight publication requires at least one active target");
	if (this->MorphWeights.size() > this->Capacity.MorphWeights || Weights.size() > this->Capacity.MorphWeights - this->MorphWeights.size())
		throw std::overflow_error("Scene collection morph-weight capacity exceeded");
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
	if (this->PickTable == nullptr)
		throw std::logic_error("Scene collection must begin a frame before adding lights");
	if (this->DirectionalLights.size() >= this->Capacity.DirectionalLights)
		throw std::overflow_error("Scene collection directional-light capacity exceeded");
	DirectionalLights.push_back(Light);
}
void SceneCollection::AddPointLight(const PointLightSource &Light)
{
	if (Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (this->PickTable == nullptr)
		throw std::logic_error("Scene collection must begin a frame before adding lights");
	if (this->PointLights.size() >= this->Capacity.PointLights)
		throw std::overflow_error("Scene collection point-light capacity exceeded");
	PointLights.push_back(Light);
}
void SceneCollection::AddSpotLight(const SpotLightSource &Light)
{
	if (Sealed)
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	if (this->PickTable == nullptr)
		throw std::logic_error("Scene collection must begin a frame before adding lights");
	if (this->SpotLights.size() >= this->Capacity.SpotLights)
		throw std::overflow_error("Scene collection spot-light capacity exceeded");
	SpotLights.push_back(Light);
}
void SceneCollection::Seal()
{
	if (this->PickTable == nullptr)
		throw std::logic_error("Scene collection must begin a frame before sealing");
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
	PickTable.reset();
}

std::vector<resource::AssetPtr<resource::Asset>> SceneCollection::ReleaseAssetPins() noexcept
{
	std::vector<resource::AssetPtr<resource::Asset>> Pins;
	this->ReleaseAssetPinsInto(Pins);
	return Pins;
}

void SceneCollection::ReleaseAssetPinsInto(std::vector<resource::AssetPtr<resource::Asset>> &Destination) noexcept
{
	Destination.clear();
	Destination.swap(this->AssetPins);
}
uint64 SceneCollection::GetFrameNumber() const noexcept
{
	return FrameNumber;
}
bool SceneCollection::IsSealed() const noexcept
{
	return Sealed;
}
const std::vector<pipeline::render::RenderItem> &SceneCollection::GetRenderItems() const noexcept
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
const std::vector<pipeline::render::GPUSkinMatrixRecord> &SceneCollection::GetSkinningMatrices() const noexcept
{
	return SkinningMatrices;
}
const std::vector<pipeline::render::GPUMorphWeightRecord> &SceneCollection::GetMorphWeights() const noexcept
{
	return MorphWeights;
}

std::shared_ptr<const pipeline::render::FramePickTable> SceneCollection::GetPickTable() const noexcept
{
	return this->PickTable;
}
