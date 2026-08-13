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

vec3 linearToSrgb(vec3 linearColor)
{
	vec3 low = linearColor * 12.92;
	vec3 high = 1.055 * pow(max(linearColor, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
	return mix(high, low, lessThanEqual(linearColor, vec3(0.0031308)));
}

void main()
{
	float exposure = texelFetch(exposureTexture, ivec2(0), 0).r;
	vec3 hdr = (texture(sceneTexture, textureCoordinate).rgb + texture(bloomTexture, textureCoordinate).rgb) * exposure;
	vec3 mapped = aces(hdr);
#ifdef ENGINE_MANUAL_SRGB_ENCODE
	mapped = linearToSrgb(mapped);
#endif
    pixelColor = vec4(mapped, 1.0);
}
