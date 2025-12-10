#version 460 core
#extension GL_ARB_gpu_shader_int64 : enable

#define POINT 0
#define SPOT 1
struct LightSource {
	vec3 position;
    float pad6;

	//-----------16-------

	vec3 direction;
    float pad7;

    //-----------32-------

    float cutOff;
    float outerCutOff;
    int pad8;
    int pad9;

	//-----------48-------

    vec3 ambient;
    float pad10;

	//------------64-------

    vec3 diffuse;
    float pad11;

	//------------80-------

    vec3 specular;
    float pad12;

	//------------96-------

    float constant;
    float linear;
    float quadratic;
    float pad13;

	//------------112-------
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