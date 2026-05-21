#include "StaticMeshObject.h"

StaticMeshObject::StaticMeshObject(StaticMesh* gameObject,
	double x,
	double y,
	double z
) : x(x), y(y), z(z), rotX(0), rotY(0), rotZ(0), scaleX(1), scaleY(1), scaleZ(1), staticGeometry(gameObject)
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

void StaticMeshObject::setPosition(float x, float y, float z) {
	this->x = x;
	this->y = y;
	this->z = z;
}

void StaticMeshObject::setRotation(float rotX, float rotY, float rotZ) {
	this->rotX = rotX;
	this->rotY = rotY;
	this->rotZ = rotZ;
}

void StaticMeshObject::setScale(float scaleX, float scaleY, float scaleZ) {
	this->scaleX = scaleX;
	this->scaleY = scaleY;
	this->scaleZ = scaleZ;
}

void StaticMeshObject::setScale(float xyz) {
	this->setScale(xyz, xyz, xyz);
}