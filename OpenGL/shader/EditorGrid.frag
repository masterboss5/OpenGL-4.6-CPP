#version 460 core

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

in vec3 nearWorld;
in vec3 farWorld;
layout(location = 0) out vec4 outputColor;

float GridLine(const vec2 position, const float spacing)
{
	const vec2 coordinate = position / spacing;
	const vec2 derivative = max(fwidth(coordinate), vec2(1.0e-5));
	const vec2 distanceToLine = abs(fract(coordinate - 0.5) - 0.5) / derivative;
	return 1.0 - min(min(distanceToLine.x, distanceToLine.y), 1.0);
}

float GridVisibility(const vec2 position, const float spacing)
{
	const vec2 derivative = fwidth(position / spacing);
	return 1.0 - smoothstep(0.18, 0.85, max(derivative.x, derivative.y));
}

void main()
{
	const float denominator = farWorld.y - nearWorld.y;
	if (abs(denominator) < 1.0e-6)
		discard;
	const float intersection = -nearWorld.y / denominator;
	if (intersection <= 0.0 || intersection >= 1.0)
		discard;

	const vec3 worldPosition = mix(nearWorld, farWorld, intersection);
	const vec4 clipPosition = viewProjection * vec4(worldPosition, 1.0);
	if (clipPosition.w <= 0.00001 || clipPosition.z < 0.0)
		discard;
	gl_FragDepth = clipPosition.z / clipPosition.w;

	const float minorLine = GridLine(worldPosition.xz, 1.0) * GridVisibility(worldPosition.xz, 1.0);
	const float majorLine = GridLine(worldPosition.xz, 10.0) * GridVisibility(worldPosition.xz, 10.0);
	const float axisX = 1.0 - min(abs(worldPosition.z) / max(fwidth(worldPosition.z), 1.0e-5), 1.0);
	const float axisZ = 1.0 - min(abs(worldPosition.x) / max(fwidth(worldPosition.x), 1.0e-5), 1.0);
	const float cameraDistance = length(worldPosition.xz - cameraPositionAndNear.xz);
	const vec3 toWorld = worldPosition - cameraPositionAndNear.xyz;
	const float toWorldLengthSquared = dot(toWorld, toWorld);
	const vec3 viewDirection = toWorldLengthSquared > 1.0e-10 ? toWorld * inversesqrt(toWorldLengthSquared) : vec3(0.0, -1.0, 0.0);
	const float grazingFade = smoothstep(0.025, 0.18, abs(viewDirection.y));
	const float fade = (1.0 - smoothstep(30.0, 180.0, cameraDistance)) * grazingFade;
	const float alpha = max(minorLine * 0.055, majorLine * 0.16) * fade;
	if (alpha <= 0.001)
		discard;

	vec3 color = mix(vec3(0.22, 0.24, 0.28), vec3(0.35, 0.38, 0.43), majorLine);
	color = mix(color, vec3(0.82, 0.20, 0.16), axisX);
	color = mix(color, vec3(0.18, 0.38, 0.88), axisZ);
	outputColor = vec4(color, max(alpha, max(axisX, axisZ) * fade * 0.6));
}
