#pragma once
#include "StaticMesh.h"
#include "glm.hpp"
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include <iostream>

class StaticMeshObject final {
private:
	float x;
	float y;
	float z;
	float rotX = 0;
	float rotY = 0;
	float rotZ = 0;
	float scaleX = 1;
	float scaleY = 1;
	float scaleZ = 1;
	const StaticMesh* const staticGeometry;
public:
	StaticMeshObject(StaticMesh* gameObject, double x, double y, double z);
	const StaticMesh* const getGameObject() const;
	void buildMatrix(glm::mat4& transformMatrix) const;
	void setPosition(float x, float y, float z);
	void setRotation(float rotX, float rotY, float rotZ);
	void setScale(float x, float y, float z);
	void setScale(float xyz);
};