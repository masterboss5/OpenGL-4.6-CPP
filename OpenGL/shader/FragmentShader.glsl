#version 460 core
#extension GL_ARB_gpu_shader_int64 : enable

const vec3 lightColor = vec3(1f, 1f, 1f);
const float ambientStrength = 0.1;


#define POINT 0
#define SPOT 1
struct LightSource {
	int type; //4
	bool isActive; //1

	vec3 position; //12
	vec3 direction; //12
	float cutOff;//4
	float outerCutOff;//4

	vec3 ambient; //12
	vec3 diffuse; //12
	vec3 specular; //12

	float constant;//4
	float linear;//4
	float quadratic;//4
};

struct Material {
	sampler2D diffuseTexture;
	sampler2D specularTexture;
	sampler2D normalTexture;
	sampler2D heightTexture;
	sampler2D ambientOcclusionTexture;
	sampler2D roughnessTexture;
	sampler2D emissiveTexture;
};

in vec2 pass_uv;
in vec3 pass_normal;
in vec3 fragPosition;

out vec4 pixelColor;

layout(std430, binding = 1) buffer LightBuffer {
    LightSource lightSources[];
};

uniform int lightCount;
uniform vec3 viewPos;
uniform Material material;

void main() {

	vec3 ambient = texture(material.diffuseTexture, pass_uv).rgb * lightSources[0].ambient;

	vec3 fragNormal = normalize(pass_normal);
	vec3 L = normalize(lightSources[0].position - fragPosition);
	float diff = max(dot(fragNormal, L), 0.0);
	vec3 diffuse = diff * lightSources[0].diffuse * texture(material.diffuseTexture, pass_uv).rgb;

	//specular
	vec3 viewDir = normalize(viewPos - fragPosition);
	vec3 reflectDir = reflect(-L, fragNormal); 
	float spec = pow(max(dot(viewDir, reflectDir), 0.0), 16);
	vec3 specular = texture(material.specularTexture, pass_uv).rgb * spec * lightSources[0].specular;

	float theta = dot(L, normalize(-lightSources[0].direction));
	float epsilon = lightSources[0].cutOff - lightSources[0].outerCutOff;
	float intensity = clamp((theta - lightSources[0].outerCutOff) / epsilon, 0.0, 1.0);
	diffuse *= intensity;
	specular *= intensity;


	float distance = length(lightSources[0].position - fragPosition);
	float attenuation = 1.0 / (lightSources[0].constant + lightSources[0].linear * distance + lightSources[0].quadratic * (distance * distance));
	ambient *= attenuation; 
	diffuse *= attenuation;
	specular *= attenuation; 


    pixelColor = vec4(ambient + diffuse + specular, 1.0);
	//pixelColor = vec4(vec3((theta+1.0)/2.0), 1.0);
}