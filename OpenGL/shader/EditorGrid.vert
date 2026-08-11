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

out vec3 nearWorld;
out vec3 farWorld;

void main()
{
	const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
	const vec2 ndc = positions[gl_VertexID];
	const vec4 nearPoint = inverseViewProjection * vec4(ndc, 1.0, 1.0);
	const vec4 farPoint = inverseViewProjection * vec4(ndc, 0.0, 1.0);
	nearWorld = nearPoint.xyz / nearPoint.w;
	farWorld = farPoint.xyz / farPoint.w;
	gl_Position = vec4(ndc, 0.0, 1.0);
}
