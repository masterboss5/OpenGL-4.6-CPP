#version 460 core

layout(binding = 0) uniform sampler2D sceneTexture;
layout(binding = 1) uniform sampler2D bloomTexture;
layout(binding = 2) uniform sampler2D exposureTexture;
in vec2 textureCoordinate;
out vec4 pixelColor;

vec3 aces(vec3 value)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((value * (a * value + b)) / (value * (c * value + d) + e), 0.0, 1.0);
}

void main()
{
	float exposure = texelFetch(exposureTexture, ivec2(0), 0).r;
	vec3 hdr = (texture(sceneTexture, textureCoordinate).rgb + texture(bloomTexture, textureCoordinate).rgb) * exposure;
    pixelColor = vec4(pow(aces(hdr), vec3(1.0 / 2.2)), 1.0);
}
