#pragma once
#include "Texture.h"
#include "ShaderProgram.h"

class Material final {
public:
	Texture diffuseTexture;
	Texture specularTexture;
	Texture normalTexture;
	Texture heightTexture;
	Texture ambientOcclusionTexture;
	Texture roughnessTexture;
	Texture emissiveTexture;

	Material(
		const Texture& diffuse,
		const Texture& specular,
		const Texture& normal,
		const Texture& height,
		const Texture& ao,
		const Texture& roughness,
		const Texture& emissive
	);
	Material(const Material&) = default;
	Material(Material&&) = default;
	Material& operator=(const Material&) = delete;
	Material& operator=(Material&&) = delete;

	void applyToShader(const ShaderProgram& shader) const;
};