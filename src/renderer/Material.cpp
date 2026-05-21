#include "PBRMaterial.h"

Material::Material(
	const Texture& diffuse,
	const Texture& specular,
	const Texture& normal,
	const Texture& height,
	const Texture& ao,
	const Texture& roughness,
	const Texture& emissive,
	const float shininess
) : diffuseTexture(diffuse), specularTexture(specular), normalTexture(normal),
heightTexture(height), ambientOcclusionTexture(ao), roughnessTexture(roughness),
emissiveTexture(emissive), shininess(shininess)
{

}

void Material::applyToShader(const ShaderProgram& shader) const
{
	shader.bind();
	glUniformHandleui64ARB(shader.getUniformLocation("material.diffuseTexture"), this->diffuseTexture.getHandle());
	glUniformHandleui64ARB(shader.getUniformLocation("material.specularTexture"), this->specularTexture.getHandle());
	glUniformHandleui64ARB(shader.getUniformLocation("material.normalTexture"), this->normalTexture.getHandle());
	glUniformHandleui64ARB(shader.getUniformLocation("material.heightTexture"), this->heightTexture.getHandle());
	glUniformHandleui64ARB(shader.getUniformLocation("material.ambientOcclusionTexture"), this->ambientOcclusionTexture.getHandle());
	glUniformHandleui64ARB(shader.getUniformLocation("material.roughnessTexture"), this->roughnessTexture.getHandle());
	glUniformHandleui64ARB(shader.getUniformLocation("material.emissiveTexture"), this->emissiveTexture.getHandle());
	glUniform1f(shader.getUniformLocation("material.shininess"), this->shininess);
}