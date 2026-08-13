#version 460 core

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

struct DebugLineRecord
{
	vec4 startAndWidth;
	vec4 end;
	vec4 color;
};
layout(std430, binding = 13) readonly buffer DebugLines
{
	DebugLineRecord lines[];
};

flat out vec4 lineColor;

bool ClipLineToNearPlane(inout vec4 startClip, inout vec4 endClip)
{
	const float minimumW = 0.00001;
	float startDistances[2] = float[2](startClip.z, startClip.w - minimumW);
	float endDistances[2] = float[2](endClip.z, endClip.w - minimumW);
	for (uint plane = 0U; plane < 2U; ++plane)
	{
		if (startDistances[plane] < 0.0 && endDistances[plane] < 0.0)
			return false;
		if (startDistances[plane] < 0.0 || endDistances[plane] < 0.0)
		{
			float interpolation = startDistances[plane] / (startDistances[plane] - endDistances[plane]);
			vec4 clipped = mix(startClip, endClip, interpolation);
			if (startDistances[plane] < 0.0)
				startClip = clipped;
			else
				endClip = clipped;
			startDistances[0] = startClip.z;
			startDistances[1] = startClip.w - minimumW;
			endDistances[0] = endClip.z;
			endDistances[1] = endClip.w - minimumW;
		}
	}
	return true;
}

void main()
{
	const uint lineIndex = uint(gl_VertexID) / 6U;
	const uint corner = uint(gl_VertexID) % 6U;
	const DebugLineRecord line = lines[lineIndex];
	vec4 startClip = viewProjection * vec4(line.startAndWidth.xyz, 1.0);
	vec4 endClip = viewProjection * vec4(line.end.xyz, 1.0);
	if (!ClipLineToNearPlane(startClip, endClip))
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		lineColor = vec4(0.0);
		return;
	}
	const vec2 startNdc = startClip.xy / startClip.w;
	const vec2 endNdc = endClip.xy / endClip.w;
	const vec2 directionPixels = (endNdc - startNdc) * renderExtentAndFar.xy;
	const vec2 perpendicular = length(directionPixels) > 0.0001 ? normalize(vec2(-directionPixels.y, directionPixels.x)) : vec2(0.0, 1.0);
	const vec2 offsetNdc = perpendicular * line.startAndWidth.w * 2.0 / renderExtentAndFar.xy;
	const bool useEnd = corner == 1U || corner == 2U || corner == 4U;
	const bool positiveSide = corner == 0U || corner == 1U || corner == 3U;
	const vec4 endpoint = useEnd ? endClip : startClip;
	gl_Position = endpoint;
	gl_Position.xy += (positiveSide ? offsetNdc : -offsetNdc) * endpoint.w;
	lineColor = line.color;
}
