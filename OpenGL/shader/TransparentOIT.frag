#version 460 core
#extension GL_ARB_bindless_texture : require
#extension GL_ARB_gpu_shader_int64 : require

layout(location = 0) out vec4 accumulationOutput;
layout(location = 1) out float revealageOutput;

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
    vec4 color = material.baseColorFactor;
	if (material.baseColorTexture != uint64_t(0)) color *= texture(sampler2D(material.baseColorTexture), materialTextureCoordinate(material, 0u));
    float alpha = clamp(color.a, 0.0, 1.0);
    // McGuire/Bavoil weighted blended transparency. With reversed-Z, depth
    // increases toward the viewer, so close fragments receive more weight.
    float depthWeight = pow(clamp(gl_FragCoord.z, 0.0, 1.0), 3.0);
    float alphaWeight = pow(min(1.0, alpha * 10.0) + 0.01, 3.0);
    float weight = clamp(alphaWeight * 1.0e8 * depthWeight, 0.01, 3000.0);
    accumulationOutput = vec4(color.rgb * alpha, alpha) * weight;
    revealageOutput = alpha;
}
