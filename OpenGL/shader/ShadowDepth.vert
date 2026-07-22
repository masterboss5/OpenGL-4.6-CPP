#version 460 core

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 uv0;
layout(location = 3) in vec2 uv1;
layout(location = 4) in vec2 uv2;
layout(location = 5) in vec2 uv3;
layout(location = 12) in uvec4 jointIndices;
layout(location = 13) in vec4 jointWeights;

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
	uint skinPaletteOffset;
	uint previousSkinPaletteOffset;
    uint flags;
	uint morphWeightOffset;
	uint morphWeightCount;
};
struct ShadowRecord
{
    mat4 viewProjection;
    vec4 atlasScaleBias;
    vec4 depthBiasAndFilter;
};

layout(std430, binding = 0) readonly buffer InstanceData { InstanceRecord instances[]; };
layout(std430, binding = 8) readonly buffer ShadowData { ShadowRecord shadows[]; };
struct SkinMatrixRecord { mat4 current; mat4 previous; };
layout(std430, binding = 9) readonly buffer SkinMatrices { SkinMatrixRecord skinMatrices[]; };
struct MorphDeltaRecord { vec4 positionDelta; vec4 normalDelta; };
struct MorphWeightRecord { uint deltaOffset; float currentWeight; float previousWeight; uint padding; };
layout(std430, binding = 10) readonly buffer MorphDeltas { MorphDeltaRecord morphDeltas[]; };
layout(std430, binding = 11) readonly buffer MorphWeights { MorphWeightRecord morphWeights[]; };
uniform uint shadowViewIndex;

out SHADOW_OUT
{
	vec2 textureCoordinates[4];
	flat uint materialIndex;
	flat uint instanceFlags;
} outputData;

void main()
{
    InstanceRecord instance = instances[gl_BaseInstance + gl_InstanceID];
	outputData.textureCoordinates[0] = uv0;
	outputData.textureCoordinates[1] = uv1;
	outputData.textureCoordinates[2] = uv2;
	outputData.textureCoordinates[3] = uv3;
	outputData.materialIndex = instance.materialIndex;
	outputData.instanceFlags = instance.flags;
	ShadowRecord shadow = shadows[shadowViewIndex];
	vec4 clipCenter = shadow.viewProjection * vec4(instance.worldBounds.xyz, 1.0);
	float conservativeRadius = instance.worldBounds.w *
		max(abs(shadow.viewProjection[0][0]), max(abs(shadow.viewProjection[1][1]), abs(shadow.viewProjection[2][2])));
	bool visible = clipCenter.x >= -clipCenter.w - conservativeRadius && clipCenter.x <= clipCenter.w + conservativeRadius &&
		clipCenter.y >= -clipCenter.w - conservativeRadius && clipCenter.y <= clipCenter.w + conservativeRadius &&
		clipCenter.z >= -conservativeRadius && clipCenter.z <= clipCenter.w + conservativeRadius;
	if (!visible)
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	vec3 deformedPosition = position;
	if ((instance.flags & 4u) != 0u)
	{
		for (uint morphIndex = 0u; morphIndex < instance.morphWeightCount; ++morphIndex)
		{
			MorphWeightRecord morph = morphWeights[instance.morphWeightOffset + morphIndex];
			deformedPosition += morphDeltas[morph.deltaOffset + uint(gl_VertexID)].positionDelta.xyz * morph.currentWeight;
		}
	}
	mat4 skin = mat4(1.0);
	if ((instance.flags & 2u) != 0u)
	{
		skin = skinMatrices[instance.skinPaletteOffset + jointIndices.x].current * jointWeights.x +
			skinMatrices[instance.skinPaletteOffset + jointIndices.y].current * jointWeights.y +
			skinMatrices[instance.skinPaletteOffset + jointIndices.z].current * jointWeights.z +
			skinMatrices[instance.skinPaletteOffset + jointIndices.w].current * jointWeights.w;
	}
	gl_Position = shadow.viewProjection * instance.transform * skin * vec4(deformedPosition, 1.0);
}
