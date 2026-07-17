#pragma once
#include "src/renderer/StaticMesh.h"
#include "glm.hpp"
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include <iostream>

#include "src/types.h"

class StaticMeshObject final {
private:
	float32 x;
	float32 y;
	float32 z;
	float32 rotX = 0.0f;
	float32 rotY = 0.0f;
	float32 rotZ = 0.0f;
	float32 scaleX = 1.0f;
	float32 scaleY = 1.0f;
	float32 scaleZ = 1.0f;
	const StaticMesh* const staticGeometry;
public:
	StaticMeshObject(StaticMesh* gameObject, float32 x, float32 y, float32 z);
	const StaticMesh* const getGameObject() const;
	void buildMatrix(glm::mat4& transformMatrix) const;
	void setPosition(float32 x, float32 y, float32 z);
	void setRotation(float32 rotX, float32 rotY, float32 rotZ);
	void setScale(float32 x, float32 y, float32 z);
	void setScale(float32 xyz);
};
