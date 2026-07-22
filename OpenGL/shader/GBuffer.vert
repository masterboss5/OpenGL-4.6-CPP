#version 460 core

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv0;
layout(location = 3) in vec2 uv1;
layout(location = 4) in vec2 uv2;
layout(location = 5) in vec2 uv3;
layout(location = 8) in vec4 tangent;
layout(location = 12) in uvec4 jointIndices;
layout(location = 13) in vec4 jointWeights;

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
	vec4 backgroundColor;
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
layout(std430, binding = 0) readonly buffer InstanceData { InstanceRecord instances[]; };

struct SkinMatrixRecord
{
	mat4 current;
	mat4 previous;
};
layout(std430, binding = 9) readonly buffer SkinMatrices { SkinMatrixRecord skinMatrices[]; };

struct MorphDeltaRecord { vec4 positionDelta; vec4 normalDelta; };
struct MorphWeightRecord { uint deltaOffset; float currentWeight; float previousWeight; uint padding; };
layout(std430, binding = 10) readonly buffer MorphDeltas { MorphDeltaRecord morphDeltas[]; };
layout(std430, binding = 11) readonly buffer MorphWeights { MorphWeightRecord morphWeights[]; };

out VS_OUT
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
} outputData;

void main()
{
    InstanceRecord instance = instances[gl_BaseInstance + gl_InstanceID];
	vec3 currentPosition = position;
	vec3 previousPosition = position;
	vec3 currentNormal = normal;
	if ((instance.flags & 4u) != 0u)
	{
		for (uint morphIndex = 0u; morphIndex < instance.morphWeightCount; ++morphIndex)
		{
			MorphWeightRecord morph = morphWeights[instance.morphWeightOffset + morphIndex];
			MorphDeltaRecord delta = morphDeltas[morph.deltaOffset + uint(gl_VertexID)];
			currentPosition += delta.positionDelta.xyz * morph.currentWeight;
			previousPosition += delta.positionDelta.xyz * morph.previousWeight;
			currentNormal += delta.normalDelta.xyz * morph.currentWeight;
		}
		currentNormal = normalize(currentNormal);
	}
	mat4 currentSkin = mat4(1.0);
	mat4 previousSkin = mat4(1.0);
	if ((instance.flags & 2u) != 0u)
	{
		currentSkin = skinMatrices[instance.skinPaletteOffset + jointIndices.x].current * jointWeights.x +
			skinMatrices[instance.skinPaletteOffset + jointIndices.y].current * jointWeights.y +
			skinMatrices[instance.skinPaletteOffset + jointIndices.z].current * jointWeights.z +
			skinMatrices[instance.skinPaletteOffset + jointIndices.w].current * jointWeights.w;
		previousSkin = skinMatrices[instance.previousSkinPaletteOffset + jointIndices.x].previous * jointWeights.x +
			skinMatrices[instance.previousSkinPaletteOffset + jointIndices.y].previous * jointWeights.y +
			skinMatrices[instance.previousSkinPaletteOffset + jointIndices.z].previous * jointWeights.z +
			skinMatrices[instance.previousSkinPaletteOffset + jointIndices.w].previous * jointWeights.w;
	}
	vec4 localPosition = currentSkin * vec4(currentPosition, 1.0);
	vec4 worldPosition = instance.transform * localPosition;
    outputData.worldPosition = worldPosition.xyz;
	mat3 normalTransform = mat3(transpose(inverse(instance.transform * currentSkin)));
	outputData.worldNormal = normalize(normalTransform * currentNormal);
	outputData.worldTangent = vec4(normalize(normalTransform * tangent.xyz), tangent.w);
    outputData.textureCoordinates[0] = uv0;
    outputData.textureCoordinates[1] = uv1;
    outputData.textureCoordinates[2] = uv2;
    outputData.textureCoordinates[3] = uv3;
    outputData.currentClip = viewProjection * worldPosition;
	outputData.previousClip = previousViewProjection * instance.previousTransform * previousSkin * vec4(previousPosition, 1.0);
    outputData.materialIndex = instance.materialIndex;
    outputData.objectID = instance.objectID;
	outputData.instanceFlags = instance.flags;
    gl_Position = outputData.currentClip;
}
