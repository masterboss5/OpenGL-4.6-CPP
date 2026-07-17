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

