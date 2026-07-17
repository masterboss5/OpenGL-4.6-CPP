#include "SceneCollection.h"

#include <stdexcept>

#include "StaticMeshObject.h"
#include "src/renderer/StaticMesh.h"

void SceneCollection::beginFrame(uint64 newFrameNumber)
{
	this->clear();
	this->frameNumber = newFrameNumber;
}

void SceneCollection::submit(const StaticMeshObject& object, uint32 objectID, uint32 layerMask, uint64 revision, bool transparent)
{
	const StaticMesh* mesh = object.getGameObject();
	if (mesh == nullptr)
	{
		throw std::invalid_argument("Cannot submit a StaticMeshObject without a StaticMesh");
	}

	glm::mat4 transform { 1.0f };
	object.buildMatrix(transform);
	const std::vector<float>& vertices = mesh->getVertices();
	glm::vec3 minimum { 0.0f };
	glm::vec3 maximum { 0.0f };
	if (!vertices.empty())
	{
		minimum = glm::vec3(vertices[0], vertices[1], vertices[2]);
		maximum = minimum;
	for (std::size_t offset = 3; offset + 2 < vertices.size(); offset += 3)
		{
			const glm::vec3 position { vertices[offset], vertices[offset + 1], vertices[offset + 2] };
			minimum = glm::min(minimum, position);
			maximum = glm::max(maximum, position);
		}
	}
	const glm::vec3 localCenter = (minimum + maximum) * 0.5f;
	const glm::vec3 localExtent = (maximum - minimum) * 0.5f;
	const glm::vec3 worldCenter = glm::vec3(transform * glm::vec4(localCenter, 1.0f));
	const glm::vec3 scale { glm::length(glm::vec3(transform[0])), glm::length(glm::vec3(transform[1])), glm::length(glm::vec3(transform[2])) };
	const float32 radius = glm::length(localExtent * scale);
	this->submit({ .mesh = mesh, .transform = transform, .worldBounds = glm::vec4(worldCenter, radius), .objectID = objectID, .layerMask = layerMask, .revision = revision, .transparent = transparent });
}

void SceneCollection::submit(renderer::RenderItem item)
{
	if (this->sealed)
	{
		throw std::logic_error("SceneCollection cannot be modified after it is sealed");
	}
	if (item.mesh == nullptr)
	{
		throw std::invalid_argument("RenderItem requires a mesh");
	}
	this->renderItems.push_back(std::move(item));
}

void SceneCollection::addDirectionalLight(const DirectionalLightSource& light) { if (sealed) throw std::logic_error("SceneCollection cannot be modified after it is sealed"); directionalLights.push_back(light); }
void SceneCollection::addPointLight(const PointLightSource& light) { if (sealed) throw std::logic_error("SceneCollection cannot be modified after it is sealed"); pointLights.push_back(light); }
void SceneCollection::addSpotLight(const SpotLightSource& light) { if (sealed) throw std::logic_error("SceneCollection cannot be modified after it is sealed"); spotLights.push_back(light); }
void SceneCollection::seal() { this->sealed = true; }
void SceneCollection::clear() { this->sealed = false; renderItems.clear(); directionalLights.clear(); pointLights.clear(); spotLights.clear(); }
uint64 SceneCollection::getFrameNumber() const noexcept { return frameNumber; }
bool SceneCollection::isSealed() const noexcept { return sealed; }
const std::vector<renderer::RenderItem>& SceneCollection::getRenderItems() const noexcept { return renderItems; }
const std::vector<DirectionalLightSource>& SceneCollection::getDirectionalLights() const noexcept { return directionalLights; }
const std::vector<PointLightSource>& SceneCollection::getPointLights() const noexcept { return pointLights; }
const std::vector<SpotLightSource>& SceneCollection::getSpotLights() const noexcept { return spotLights; }
