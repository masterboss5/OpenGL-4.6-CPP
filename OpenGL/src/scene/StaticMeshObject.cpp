#include "StaticMeshObject.h"

StaticMeshObject::StaticMeshObject(StaticMesh* gameObject,
	float32 x,
	float32 y,
	float32 z
) : x(x), y(y), z(z), rotX(0.0f), rotY(0.0f), rotZ(0.0f), scaleX(1.0f), scaleY(1.0f), scaleZ(1.0f), staticGeometry(gameObject)
{
}

void StaticMeshObject::buildMatrix(glm::mat4& transformMatrix) const {
	transformMatrix = glm::translate(transformMatrix, glm::vec3(x, y, z));

	transformMatrix = glm::rotate(transformMatrix, glm::radians(rotX), glm::vec3(1.0f, 0.0f, 0.0f));
	transformMatrix = glm::rotate(transformMatrix, glm::radians(rotY), glm::vec3(0.0f, 1.0f, 0.0f));
	transformMatrix = glm::rotate(transformMatrix, glm::radians(rotZ), glm::vec3(0.0f, 0.0f, 1.0f));

	transformMatrix = glm::scale(transformMatrix, glm::vec3(this->scaleX, this->scaleY, this->scaleZ));
}

const StaticMesh* const StaticMeshObject::getGameObject() const {
	return this->staticGeometry;
}

void StaticMeshObject::setPosition(float32 x, float32 y, float32 z) {
	this->x = x;
	this->y = y;
	this->z = z;
}

void StaticMeshObject::setRotation(float32 rotX, float32 rotY, float32 rotZ) {
	this->rotX = rotX;
	this->rotY = rotY;
	this->rotZ = rotZ;
}

void StaticMeshObject::setScale(float32 scaleX, float32 scaleY, float32 scaleZ) {
	this->scaleX = scaleX;
	this->scaleY = scaleY;
	this->scaleZ = scaleZ;
}

void StaticMeshObject::setScale(float32 xyz) {
	this->setScale(xyz, xyz, xyz);
}
