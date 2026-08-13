#version 460 core
#extension GL_ARB_bindless_texture : require
#extension GL_ARB_gpu_shader_int64 : require

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
layout(std430, binding = 1) readonly buffer Materials { MaterialRecord materials[]; };

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
} inputData;

vec2 baseColorTextureCoordinate(MaterialRecord material)
{
	uint coordinateIndex = uint(material.textureCoordinateSelectors & uint64_t(0xFu));
	return inputData.textureCoordinates[min(coordinateIndex, 3u)];
}

void main()
{
	if ((inputData.instanceFlags & 32U) == 0U) return;
	MaterialRecord material = materials[inputData.materialIndex];
	float alpha = material.baseColorFactor.a;
	if (material.baseColorTexture != uint64_t(0))
		alpha *= texture(sampler2D(material.baseColorTexture), baseColorTextureCoordinate(material)).a;
	if (alpha < material.roughnessTransmissionIor.w) discard;
}
