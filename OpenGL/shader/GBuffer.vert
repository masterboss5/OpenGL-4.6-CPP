#version 460 core

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

out gl_PerVertex
{
    vec4 gl_Position;
};

layout(std140, binding = 0) uniform FrameConstants
{
    mat4 projection;
    mat4 view;
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 inverseViewProjection;
    vec4 cameraPositionAndNear;
    vec4 renderExtentAndFar;
    uvec4 countsAndFrame;
};

struct InstanceRecord
{
    mat4 transform;
    mat4 previousTransform;
    vec4 worldBounds;
    uint materialIndex;
    uint objectID;
    uint batchIndex;
    uint flags;
};
layout(std430, binding = 0) readonly buffer InstanceData { InstanceRecord instances[]; };

out VS_OUT
{
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 textureCoordinate;
    vec4 currentClip;
    vec4 previousClip;
    flat uint materialIndex;
    flat uint objectID;
} outputData;

void main()
{
    InstanceRecord instance = instances[gl_BaseInstance + gl_InstanceID];
    vec4 worldPosition = instance.transform * vec4(position, 1.0);
    outputData.worldPosition = worldPosition.xyz;
    outputData.worldNormal = mat3(transpose(inverse(instance.transform))) * normal;
    outputData.textureCoordinate = uv;
    outputData.currentClip = viewProjection * worldPosition;
    outputData.previousClip = previousViewProjection * instance.previousTransform * vec4(position, 1.0);
    outputData.materialIndex = instance.materialIndex;
    outputData.objectID = instance.objectID;
    gl_Position = outputData.currentClip;
}
