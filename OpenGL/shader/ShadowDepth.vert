#version 460 core

layout(location = 0) in vec3 position;

out gl_PerVertex
{
    vec4 gl_Position;
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
struct ShadowRecord
{
    mat4 viewProjection;
    vec4 atlasScaleBias;
    vec4 depthBiasAndFilter;
};

layout(std430, binding = 0) readonly buffer InstanceData { InstanceRecord instances[]; };
layout(std430, binding = 8) readonly buffer ShadowData { ShadowRecord shadows[]; };
uniform uint shadowViewIndex;

void main()
{
    InstanceRecord instance = instances[gl_BaseInstance + gl_InstanceID];
    gl_Position = shadows[shadowViewIndex].viewProjection * instance.transform * vec4(position, 1.0);
}
