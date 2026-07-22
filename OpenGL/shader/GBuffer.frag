#version 460 core
#extension GL_ARB_bindless_texture : require
#extension GL_ARB_gpu_shader_int64 : require

layout(location = 0) out vec4 baseColorOutput;
layout(location = 1) out vec4 normalRoughnessOutput;
layout(location = 2) out vec4 materialOutput;
layout(location = 3) out vec2 velocityOutput;
layout(location = 4) out uint objectIDOutput;

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

vec2 materialTextureCoordinate(MaterialRecord material, uint semantic)
{
	uint coordinateIndex = uint((material.textureCoordinateSelectors >> (semantic * 4u)) & uint64_t(0xFu));
	return inputData.textureCoordinates[min(coordinateIndex, 3u)];
}

void main()
{
    MaterialRecord material = materials[inputData.materialIndex];
    vec4 albedo = material.baseColorFactor;
	if (material.baseColorTexture != uint64_t(0)) albedo *= texture(sampler2D(material.baseColorTexture), materialTextureCoordinate(material, 0u));
	if ((inputData.instanceFlags & 32U) != 0U && albedo.a < material.roughnessTransmissionIor.w) discard;
	vec3 surfaceNormal = normalize(inputData.worldNormal);
	if (material.normalTexture != uint64_t(0))
	{
		vec3 tangentNormal = texture(sampler2D(material.normalTexture), materialTextureCoordinate(material, 1u)).xyz * 2.0 - 1.0;
		tangentNormal.xy *= material.textureControls.x;
		vec3 tangentDirection = normalize(inputData.worldTangent.xyz - surfaceNormal * dot(inputData.worldTangent.xyz, surfaceNormal));
		vec3 bitangentDirection = normalize(cross(surfaceNormal, tangentDirection)) * inputData.worldTangent.w;
		surfaceNormal = normalize(mat3(tangentDirection, bitangentDirection, surfaceNormal) * tangentNormal);
	}
	float roughness = material.roughnessTransmissionIor.x;
	float metallic = material.emissiveAndMetallic.w;
	if (material.metallicRoughnessTexture != uint64_t(0))
	{
		vec4 metallicRoughness = texture(sampler2D(material.metallicRoughnessTexture), materialTextureCoordinate(material, 2u));
		roughness *= metallicRoughness.g;
		metallic *= metallicRoughness.b;
	}
	vec3 emissive = material.emissiveAndMetallic.xyz;
	if (material.emissiveTexture != uint64_t(0))
		emissive *= texture(sampler2D(material.emissiveTexture), materialTextureCoordinate(material, 4u)).rgb;
	float specular = material.textureControls.z;
	if (material.specularTexture != uint64_t(0))
	{
		vec3 specularSample = texture(sampler2D(material.specularTexture), materialTextureCoordinate(material, 5u)).rgb;
		specular *= dot(specularSample, vec3(0.2126, 0.7152, 0.0722));
	}
	vec3 encodedNormal = surfaceNormal * 0.5 + 0.5;
    vec2 currentNdc = inputData.currentClip.xy / max(inputData.currentClip.w, 0.00001);
    vec2 previousNdc = inputData.previousClip.xy / max(inputData.previousClip.w, 0.00001);
	baseColorOutput = vec4(albedo.rgb, clamp(specular, 0.0, 1.0));
	normalRoughnessOutput = vec4(encodedNormal, clamp(roughness, 0.045, 1.0));
	float storedMetallic = clamp(metallic, 0.0, 1.0);
	if ((inputData.instanceFlags & 16U) == 0U) storedMetallic = -storedMetallic - 1.0;
	materialOutput = vec4(emissive, storedMetallic);
    velocityOutput = currentNdc - previousNdc;
    objectIDOutput = inputData.objectID;
}
