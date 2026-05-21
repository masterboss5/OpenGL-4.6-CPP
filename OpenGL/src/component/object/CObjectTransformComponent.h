#pragma once

#include "src/component/object/CObjectComponent.h"
#include "src/component/Components.h"
#include <glm.hpp>
#include <cmath>
#include <limits>
#include <gtc/quaternion.hpp>

//TODO
//============================================================
/*
	1. Decide where to add grid snapping, as another component or inside this TransformComponenet
	2. Add macros to enable or disable [[unlikely]]
	3. add [[nodiscard]]
	4. add noexcept
	5. maybe: add setFromMatrix()
	6. add rotateX, etc
*/
//============================================================

namespace components
{
	class CObjectTransformComponent final : public CObjectComponent
	{
	private:
		glm::vec3 position {0.0f, 0.0f, 0.0f};
		glm::quat rotation {1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 scale {1.0f, 1.0f, 1.0f};
		mutable bool needsRecalculation{true};
		mutable glm::mat4 matrix {1.0f};

		void updateMatrix() const;
		void recalculateMatrix() const;
	public:
		inline static constexpr uint32 TYPE_ID = 
			static_cast<uint32>(CObjectComponentTypeID::CObjectTransformComponent);

		explicit CObjectTransformComponent
		(
			world::Object* object,
			const glm::vec3& position = glm::vec3{0.0f, 0.0f, 0.0f},
			const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
			const glm::vec3& scale = glm::vec3{1.0f, 1.0f, 1.0f}
		);

		CCOMPONENT_BODY(CObjectTransformComponent)


		void resetTransform();
		void resetPosition();
		void resetRotation();
		void resetScale();


		void setTransform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale);

		void setPosition(const glm::vec3& position);
		[[nodiscard]] glm::vec3 getPosition() const;
		void translate(const glm::vec3& translation);
		void translateX(float x);
		void translateY(float y);
		void translateZ(float z);
		void lerpPosition(const glm::vec3& target, float alpha);
		void setPositionX(float x);
		void setPositionY(float y);
		void setPositionZ(float z);
		
		void setRotation(const glm::quat& quat);
		void setRotationEuler(const glm::vec3& eulerAngles);
		void rotate(float angleDegrees, const glm::vec3& axis);
		[[nodiscard]] glm::quat getRotation() const;
		[[nodiscard]] glm::vec3 getRotationEuler() const;
		void slerpRotation(const glm::quat& target, float alpha);

		void setScale(const glm::vec3& scale);
		void setScale(float scale);
		[[nodiscard]] glm::vec3 getScale() const;
		void lerpScale(const glm::vec3& target, float alpha);
		void setScaleX(float x);
		void setScaleY(float y);
		void setScaleZ(float z);

		void lookAt(const glm::vec3& target, const glm::vec3& up = glm::vec3{0.0f, 1.0f, 0.0f});
		[[nodiscard]] float distanceTo(const glm::vec3& point) const;
		[[nodiscard]] float distanceToSquared(const glm::vec3& point) const;


		[[nodiscard]] bool isWithinDistance(const glm::vec3& point, float distance) const;

		[[nodiscard]] glm::vec3 getForward() const;
		[[nodiscard]] glm::vec3 getUp() const;
		[[nodiscard]] glm::vec3 getRight() const;
		[[nodiscard]] const glm::mat4& getMatrix() const;

		void onAttachment() override;
		void onDetachment() override;
		void onRender() override;
		void onUpdate() override;
	};
}