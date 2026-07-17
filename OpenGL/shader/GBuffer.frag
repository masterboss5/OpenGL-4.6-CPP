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
    uint64_t transmissionTexture;
    vec4 baseColorFactor;
    vec4 emissiveAndMetallic;
    vec4 roughnessTransmissionIor;
};
layout(std430, binding = 1) readonly buffer Materials { MaterialRecord materials[]; };

in VS_OUT
{
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 textureCoordinate;
    vec4 currentClip;
    vec4 previousClip;
    flat uint materialIndex;
    flat uint objectID;
} inputData;

void main()
{
    MaterialRecord material = materials[inputData.materialIndex];
    vec4 albedo = material.baseColorFactor;
    if (material.baseColorTexture != uint64_t(0)) albedo *= texture(sampler2D(material.baseColorTexture), inputData.textureCoordinate);
    vec3 encodedNormal = normalize(inputData.worldNormal) * 0.5 + 0.5;
    vec2 currentNdc = inputData.currentClip.xy / max(inputData.currentClip.w, 0.00001);
    vec2 previousNdc = inputData.previousClip.xy / max(inputData.previousClip.w, 0.00001);
    baseColorOutput = vec4(albedo.rgb, albedo.a);
    normalRoughnessOutput = vec4(encodedNormal, material.roughnessTransmissionIor.x);
    materialOutput = vec4(material.emissiveAndMetallic.xyz, material.emissiveAndMetallic.w);
    velocityOutput = currentNdc - previousNdc;
    objectIDOutput = inputData.objectID;
}
