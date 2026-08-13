#version 460 core
#extension GL_ARB_bindless_texture : require
#extension GL_ARB_gpu_shader_int64 : require

layout(location = 0) out vec4 accumulationOutput;
layout(location = 1) out float revealageOutput;

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

struct MaterialRecord
{
	uint64_t baseColorTexture;
	uint64_t normalTexture;
	uint64_t metallicRoughnessTexture;
	uint64_t occlusionTexture;
	uint64_t emissiveTexture;
	uint64_t specularTexture;
	uint64_t transmissionTexture;
	uint64_t textureCoordinateSelectors;
	vec4 baseColorFactor;
	vec4 emissiveAndMetallic;
	vec4 roughnessTransmissionIor;
	vec4 textureControls;
};
layout(std430, binding = 1) readonly buffer Materials
{
	MaterialRecord materials[];
};

struct LightRecord
{
	vec4 positionAndRange;
	vec4 directionAndType;
	vec4 colorAndIntensity;
	vec4 spotAnglesAndShadow;
};
struct ClusterHeader
{
	uint offset;
	uint count;
	uint pad0;
	uint pad1;
};
struct ShadowRecord
{
	mat4 viewProjection;
	vec4 atlasScaleBias;
	vec4 depthBiasAndFilter;
	vec4 cascadeData;
};
layout(std430, binding = 2) readonly buffer Lights
{
	LightRecord lights[];
};
layout(std430, binding = 3) readonly buffer ClusterHeaders
{
	ClusterHeader headers[];
};
layout(std430, binding = 4) readonly buffer ClusterIndices
{
	uint indices[];
};
layout(std430, binding = 8) readonly buffer ShadowData
{
	ShadowRecord shadows[];
};

layout(binding = 4) uniform sampler2DArrayShadow directionalShadowMap;
layout(binding = 5) uniform sampler2DArrayShadow spotShadowMap;
layout(binding = 6) uniform samplerCubeArrayShadow pointShadowMap;
layout(binding = 7) uniform sampler2D opaqueHDR;
layout(r32ui, binding = 7) uniform uimage2D overdrawImage;

uniform uint trackOverdraw;
uniform uint lightCount;
uniform uint clusterCount;

in VS_OUT
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
}
inputData;

const float Pi = 3.14159265359;
const uint TileCountX = 32U;
const uint TileCountY = 18U;
const uint DepthSliceCount = 24U;
const uint MaxLightsPerCluster = 100U;
const uint DirectionalShadowCascadeCount = 4U;
const uint MaximumSpotShadowCount = 64U;
const uint MaximumPointShadowCount = 16U;

vec2 materialTextureCoordinate(MaterialRecord material, uint semantic)
{
	uint coordinateIndex = uint((material.textureCoordinateSelectors >> (semantic * 4u)) & uint64_t(0xFu));
	return inputData.textureCoordinates[min(coordinateIndex, 3u)];
}

float distributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
	float roughnessSquared = roughness * roughness;
	float a2 = roughnessSquared * roughnessSquared;
	float nDotH = max(dot(normal, halfVector), 0.0);
	float denominator = nDotH * nDotH * (a2 - 1.0) + 1.0;
	return a2 / max(Pi * denominator * denominator, 0.00001);
}

float geometrySchlickGGX(float nDotDirection, float roughness)
{
	float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
	return nDotDirection / max(nDotDirection * (1.0 - k) + k, 0.00001);
}

float geometrySmith(vec3 normal, vec3 viewDirection, vec3 lightDirection, float roughness)
{
	return geometrySchlickGGX(max(dot(normal, viewDirection), 0.0), roughness) *
		   geometrySchlickGGX(max(dot(normal, lightDirection), 0.0), roughness);
}

vec3 fresnelSchlick(float cosine, vec3 f0)
{
	return f0 + (1.0 - f0) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

float samplePcfShadow(sampler2DArrayShadow shadowMap, vec4 lightClip, uint layer, float bias, float filterRadius)
{
	vec3 projectionCoordinate = lightClip.xyz / max(lightClip.w, 0.00001);
	vec2 uv = projectionCoordinate.xy * 0.5 + 0.5;
	if (projectionCoordinate.z <= 0.0 || projectionCoordinate.z >= 1.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
		return 1.0;
	float radius = clamp(filterRadius, 0.0, 2.0);
	if (radius <= 0.01)
		return texture(shadowMap, vec4(uv, float(layer), projectionCoordinate.z - bias));
	vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0).xy);
	vec2 offset = texel * radius * 0.4;
	float visibility = texture(shadowMap, vec4(uv, float(layer), projectionCoordinate.z - bias)) * 4.0;
	visibility += texture(shadowMap, vec4(uv + vec2(-offset.x, 0.0), float(layer), projectionCoordinate.z - bias)) * 2.0;
	visibility += texture(shadowMap, vec4(uv + vec2( offset.x, 0.0), float(layer), projectionCoordinate.z - bias)) * 2.0;
	visibility += texture(shadowMap, vec4(uv + vec2(0.0, -offset.y), float(layer), projectionCoordinate.z - bias)) * 2.0;
	visibility += texture(shadowMap, vec4(uv + vec2(0.0,  offset.y), float(layer), projectionCoordinate.z - bias)) * 2.0;
	visibility += texture(shadowMap, vec4(uv + vec2(-offset.x, -offset.y), float(layer), projectionCoordinate.z - bias));
	visibility += texture(shadowMap, vec4(uv + vec2( offset.x, -offset.y), float(layer), projectionCoordinate.z - bias));
	visibility += texture(shadowMap, vec4(uv + vec2(-offset.x,  offset.y), float(layer), projectionCoordinate.z - bias));
	visibility += texture(shadowMap, vec4(uv + vec2( offset.x,  offset.y), float(layer), projectionCoordinate.z - bias));
	return visibility * (1.0 / 16.0);
}

float shadowBias(ShadowRecord shadow, float nDotL)
{
	return shadow.depthBiasAndFilter.x * (1.0 + shadow.depthBiasAndFilter.y * (1.0 - nDotL));
}

vec3 ambientLighting(vec3 baseColor, float metallic, vec3 f0, float occlusion)
{
	vec3 diffuseAmbient = baseColor * (1.0 - metallic) * 0.08;
	vec3 specularAmbient = f0 * mix(0.02, 0.05, 1.0 - metallic);
	return (diffuseAmbient + specularAmbient) * occlusion;
}

float sampleDirectionalCascade(vec3 worldPosition, vec3 normal, float nDotL, uint cascade)
{
	ShadowRecord shadow = shadows[cascade];
	vec3 receiverPosition = worldPosition + normal * shadow.depthBiasAndFilter.z;
	return samplePcfShadow(directionalShadowMap, shadow.viewProjection * vec4(receiverPosition, 1.0), cascade, shadowBias(shadow, nDotL),
						   shadow.depthBiasAndFilter.w);
}

float directionalShadow(vec3 worldPosition, vec3 normal, float viewDepth, float nDotL)
{
	if (shadows.length() == 0)
		return 1.0;
	uint cascadeCount = min(uint(clamp(shadows[0].cascadeData.z, 0.0, float(DirectionalShadowCascadeCount))),
							 uint(textureSize(directionalShadowMap, 0).z));
	if (cascadeCount == 0U || viewDepth > shadows[cascadeCount - 1U].cascadeData.y)
		return 1.0;
	uint cascade = 0U;
	while (cascade + 1U < cascadeCount && viewDepth > shadows[cascade].cascadeData.y)
		++cascade;
	float visibility = sampleDirectionalCascade(worldPosition, normal, nDotL, cascade);
	if (cascade + 1U < cascadeCount && cascade + 1U < shadows.length())
	{
		float splitNear = shadows[cascade].cascadeData.x;
		float splitFar = shadows[cascade].cascadeData.y;
		float blendWidth = max((splitFar - splitNear) * 0.1, 0.01);
		if (viewDepth > splitFar - blendWidth)
		{
			float blend = smoothstep(splitFar - blendWidth, splitFar, viewDepth);
			visibility = mix(visibility, sampleDirectionalCascade(worldPosition, normal, nDotL, cascade + 1U), blend);
		}
	}
	return visibility;
}

float spotShadow(vec3 worldPosition, vec3 normal, uint spotIndex, float nDotL)
{
	if (spotIndex >= MaximumSpotShadowCount || spotIndex >= uint(textureSize(spotShadowMap, 0).z))
		return 1.0;
	uint recordIndex = DirectionalShadowCascadeCount + spotIndex;
	if (recordIndex >= shadows.length())
		return 1.0;
	ShadowRecord shadow = shadows[recordIndex];
	vec3 receiverPosition = worldPosition + normal * shadow.depthBiasAndFilter.z;
	return samplePcfShadow(spotShadowMap, shadow.viewProjection * vec4(receiverPosition, 1.0), spotIndex, shadowBias(shadow, nDotL),
						   shadow.depthBiasAndFilter.w);
}

float pointShadow(vec3 worldPosition, vec3 normal, LightRecord light, uint pointIndex, float nDotL)
{
	if (pointIndex >= MaximumPointShadowCount || pointIndex >= uint(textureSize(pointShadowMap, 0).z))
		return 1.0;
	uint recordIndex = DirectionalShadowCascadeCount + MaximumSpotShadowCount + pointIndex * 6U;
	if (recordIndex >= shadows.length())
		return 1.0;
	ShadowRecord shadow = shadows[recordIndex];
	vec3 receiverPosition = worldPosition + normal * shadow.depthBiasAndFilter.z;
	vec3 fromLight = receiverPosition - light.positionAndRange.xyz;
	float distanceToLight = length(fromLight);
	if (distanceToLight <= 0.00001)
		return 1.0;
	float nearPlane = 0.1;
	float farPlane = shadow.cascadeData.y;
	float referenceDepth = farPlane * (distanceToLight - nearPlane) / max(distanceToLight * (farPlane - nearPlane), 0.00001);
	float bias = shadowBias(shadow, nDotL);
	vec3 direction = normalize(fromLight);
	return texture(pointShadowMap, vec4(direction, float(pointIndex)), referenceDepth - bias);
}

uint getClusterIndex(float viewDepth)
{
	uint tileX = min(uint(gl_FragCoord.x) * TileCountX / uint(renderExtentAndFar.x), TileCountX - 1U);
	uint tileY = min(uint(gl_FragCoord.y) * TileCountY / uint(renderExtentAndFar.y), TileCountY - 1U);
	float logarithmicSlice = log(viewDepth / cameraPositionAndNear.w) / log(renderExtentAndFar.z / cameraPositionAndNear.w);
	uint slice = min(uint(clamp(logarithmicSlice, 0.0, 0.99999) * float(DepthSliceCount)), DepthSliceCount - 1U);
	return min(tileX + tileY * TileCountX + slice * TileCountX * TileCountY, max(clusterCount, 1U) - 1U);
}

vec3 applyTransmission(vec3 litColor, vec3 surfaceNormal, vec3 tint, vec3 emissive, float roughness, float transmission, float ior)
{
	if (transmission <= 0.0)
		return litColor;
	vec2 screenUV = gl_FragCoord.xy / max(renderExtentAndFar.xy, vec2(1.0));
	float refractionScale = (1.0 - 1.0 / clamp(ior, 1.0, 4.0)) * (1.0 - roughness) * 0.05;
	vec2 refractedUV = clamp(screenUV + surfaceNormal.xy * refractionScale, vec2(0.0), vec2(1.0));
	vec3 transmittedRadiance = texture(opaqueHDR, refractedUV).rgb * mix(vec3(1.0), tint, 0.35);
	return mix(litColor, transmittedRadiance + emissive, transmission);
}

void main()
{
	MaterialRecord material = materials[inputData.materialIndex];
	vec4 baseColor = material.baseColorFactor;
	if (material.baseColorTexture != uint64_t(0))
		baseColor *= texture(sampler2D(material.baseColorTexture), materialTextureCoordinate(material, 0U));
	float alpha = clamp(baseColor.a, 0.0, 1.0);
	if (alpha <= 0.00001)
		discard;

	vec3 surfaceNormal = normalize(inputData.worldNormal);
	if (material.normalTexture != uint64_t(0))
	{
		vec3 tangentNormal = texture(sampler2D(material.normalTexture), materialTextureCoordinate(material, 1U)).xyz * 2.0 - 1.0;
		tangentNormal.xy *= material.textureControls.x;
		vec3 projectedTangent = inputData.worldTangent.xyz - surfaceNormal * dot(inputData.worldTangent.xyz, surfaceNormal);
		if (dot(projectedTangent, projectedTangent) <= 0.00000001)
		{
			vec3 fallbackAxis = abs(surfaceNormal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
			projectedTangent = cross(fallbackAxis, surfaceNormal);
		}
		vec3 tangentDirection = normalize(projectedTangent);
		vec3 bitangentDirection = normalize(cross(surfaceNormal, tangentDirection)) * (inputData.worldTangent.w < 0.0 ? -1.0 : 1.0);
		surfaceNormal = normalize(mat3(tangentDirection, bitangentDirection, surfaceNormal) * tangentNormal);
	}

	float roughness = clamp(material.roughnessTransmissionIor.x, 0.045, 1.0);
	float metallic = clamp(material.emissiveAndMetallic.w, 0.0, 1.0);
	if (material.metallicRoughnessTexture != uint64_t(0))
	{
		vec4 metallicRoughness = texture(sampler2D(material.metallicRoughnessTexture), materialTextureCoordinate(material, 2U));
		roughness = clamp(roughness * metallicRoughness.g, 0.045, 1.0);
		metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
	}
	float specularFactor = clamp(material.textureControls.z, 0.0, 1.0);
	if (material.specularTexture != uint64_t(0))
	{
		vec3 specularSample = texture(sampler2D(material.specularTexture), materialTextureCoordinate(material, 5U)).rgb;
		specularFactor *= dot(specularSample, vec3(0.2126, 0.7152, 0.0722));
	}
	vec3 emissive = material.emissiveAndMetallic.xyz;
	if (material.emissiveTexture != uint64_t(0))
		emissive *= texture(sampler2D(material.emissiveTexture), materialTextureCoordinate(material, 4U)).rgb;
	float occlusion = 1.0;
	if (material.occlusionTexture != uint64_t(0))
	{
		float sampledOcclusion = texture(sampler2D(material.occlusionTexture), materialTextureCoordinate(material, 3U)).r;
		occlusion = mix(1.0, clamp(sampledOcclusion, 0.0, 1.0), clamp(material.textureControls.y, 0.0, 1.0));
	}
	float transmission = clamp(material.roughnessTransmissionIor.y, 0.0, 1.0);
	if (material.transmissionTexture != uint64_t(0))
		transmission *= texture(sampler2D(material.transmissionTexture), materialTextureCoordinate(material, 8U)).r;
	transmission = clamp(transmission * (1.0 - metallic), 0.0, 1.0);

	vec3 viewDirection = normalize(cameraPositionAndNear.xyz - inputData.worldPosition);
	vec3 f0 = mix(vec3(0.04 * specularFactor), baseColor.rgb, metallic);
	float viewDepth = max(-(view * vec4(inputData.worldPosition, 1.0)).z, cameraPositionAndNear.w);
	vec3 outgoingRadiance = vec3(0.0);
	if (clusterCount == 0U)
	{
		vec3 ambient = ambientLighting(baseColor.rgb, metallic, f0, occlusion);
		vec3 litColor = applyTransmission(ambient + emissive, surfaceNormal, baseColor.rgb, emissive, roughness, transmission,
										  material.roughnessTransmissionIor.z);
		if (trackOverdraw != 0U)
			imageAtomicAdd(overdrawImage, ivec2(gl_FragCoord.xy), 1U);
		float depthWeight = pow(clamp(gl_FragCoord.z, 0.0, 1.0), 3.0);
		float alphaWeight = pow(min(1.0, alpha * 10.0) + 0.01, 3.0);
		float weight = clamp(alphaWeight * 1.0e8 * depthWeight, 0.01, 3000.0);
		accumulationOutput = vec4(litColor * alpha, alpha) * weight;
		revealageOutput = alpha;
		return;
	}
	ClusterHeader header = headers[getClusterIndex(viewDepth)];
	for (uint index = 0U; index < min(header.count, MaxLightsPerCluster); ++index)
	{
		uint lightIndex = indices[header.offset + index];
		if (lightIndex >= lightCount)
			continue;
		LightRecord light = lights[lightIndex];
		vec3 lightDirection;
		float attenuation = 1.0;
		if (light.directionAndType.w == 0.0)
		{
			lightDirection = normalize(-light.directionAndType.xyz);
		}
		else
		{
			vec3 toLight = light.positionAndRange.xyz - inputData.worldPosition;
			float distanceToLight = length(toLight);
			if (distanceToLight >= light.positionAndRange.w)
				continue;
			lightDirection = toLight / max(distanceToLight, 0.00001);
			attenuation = 1.0 / max(distanceToLight * distanceToLight, 0.01);
			attenuation *= pow(clamp(1.0 - distanceToLight / light.positionAndRange.w, 0.0, 1.0), 2.0);
			if (light.directionAndType.w == 2.0)
			{
				float spotCosine =
					dot(normalize(inputData.worldPosition - light.positionAndRange.xyz), normalize(light.directionAndType.xyz));
				attenuation *= smoothstep(light.spotAnglesAndShadow.y, light.spotAnglesAndShadow.x, spotCosine);
			}
		}

		float nDotL = max(dot(surfaceNormal, lightDirection), 0.0);
		if (nDotL <= 0.0)
			continue;
		vec3 halfVector = normalize(viewDirection + lightDirection);
		vec3 fresnel = fresnelSchlick(max(dot(halfVector, viewDirection), 0.0), f0);
		float distribution = distributionGGX(surfaceNormal, halfVector, roughness);
		float geometry = geometrySmith(surfaceNormal, viewDirection, lightDirection, roughness);
		vec3 specular = distribution * geometry * fresnel / max(4.0 * max(dot(surfaceNormal, viewDirection), 0.0) * nDotL, 0.0001);
		vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
		vec3 radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.a * attenuation;
		float shadowVisibility = 1.0;
		if (light.spotAnglesAndShadow.w >= 0.0)
		{
			uint shadowIndex = uint(light.spotAnglesAndShadow.w);
			if (light.directionAndType.w == 0.0)
				shadowVisibility = directionalShadow(inputData.worldPosition, surfaceNormal, viewDepth, nDotL);
			else if (light.directionAndType.w == 1.0)
				shadowVisibility = pointShadow(inputData.worldPosition, surfaceNormal, light, shadowIndex, nDotL);
			else if (light.directionAndType.w == 2.0)
				shadowVisibility = spotShadow(inputData.worldPosition, surfaceNormal, shadowIndex, nDotL);
		}
		outgoingRadiance += (diffuseWeight * baseColor.rgb / Pi + specular) * radiance * nDotL * shadowVisibility;
	}

	vec3 ambient = ambientLighting(baseColor.rgb, metallic, f0, occlusion);
	vec3 litColor = applyTransmission(ambient + outgoingRadiance + emissive, surfaceNormal, baseColor.rgb, emissive, roughness,
									  transmission, material.roughnessTransmissionIor.z);
	if (trackOverdraw != 0U)
		imageAtomicAdd(overdrawImage, ivec2(gl_FragCoord.xy), 1U);

	// McGuire/Bavoil weighted blended transparency. With reversed-Z, depth
	// increases toward the viewer, so close fragments receive more weight.
	float depthWeight = pow(clamp(gl_FragCoord.z, 0.0, 1.0), 3.0);
	float alphaWeight = pow(min(1.0, alpha * 10.0) + 0.01, 3.0);
	float weight = clamp(alphaWeight * 1.0e8 * depthWeight, 0.01, 3000.0);
	accumulationOutput = vec4(litColor * alpha, alpha) * weight;
	revealageOutput = alpha;
}
