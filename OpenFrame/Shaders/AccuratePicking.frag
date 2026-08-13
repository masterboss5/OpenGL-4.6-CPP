#version 460 core
#extension GL_ARB_bindless_texture : require
#extension GL_ARB_gpu_shader_int64 : require

layout(location = 0) out uint objectIDOutput;

struct MaterialRecord
{
	uint64_t baseColorTexture;
	uint64_t normalTexture;
	uint64_t metallicRoughnessTexture;
	uint64_t occlusionTexture;
	uint64_t emissiveTexture;
	uint64_t specularTexture;
	uint64_t transmissionTexture;
	uint64_t textureCoordinateSelectors;
	vec4 baseColorFactor;
	vec4 emissiveAndMetallic;
	vec4 roughnessTransmissionIor;
	vec4 textureControls;
};
layout(std430, binding = 1) readonly buffer Materials
{
	MaterialRecord materials[];
};

in VS_OUT
{
	vec3 worldPosition;
	vec3 worldNormal;
	vec4 worldTangent;
	vec2 textureCoordinates[4];
	vec4 currentClip;
	vec4 previousClip;
	flat uint materialIndex;
	flat uint objectID;
	flat uint instanceFlags;
}
inputData;

vec2 materialTextureCoordinate(MaterialRecord material, uint semantic)
{
	const uint coordinateIndex = uint((material.textureCoordinateSelectors >> (semantic * 4U)) & uint64_t(0xFU));
	return inputData.textureCoordinates[min(coordinateIndex, 3U)];
}

void main()
{
	const MaterialRecord material = materials[inputData.materialIndex];
	vec4 baseColor = material.baseColorFactor;
	if (material.baseColorTexture != uint64_t(0))
		baseColor *= texture(sampler2D(material.baseColorTexture), materialTextureCoordinate(material, 0U));

	const bool masked = (inputData.instanceFlags & 32U) != 0U;
	const bool transparent = (inputData.instanceFlags & 1U) != 0U;
	if ((masked && baseColor.a < material.roughnessTransmissionIor.w) || (transparent && baseColor.a <= (1.0 / 255.0)))
		discard;
	objectIDOutput = inputData.objectID;
}
