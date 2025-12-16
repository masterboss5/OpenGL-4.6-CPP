#version 460 core
#extension GL_ARB_gpu_shader_int64 : enable

struct SpotLightSource {
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

struct PointLightSource
{
	vec3 position;
	float pad6;

	//-----------16-------

	vec3 ambient;
	float pad10;

	//------------32-------

	vec3 diffuse;
	float pad11;

	//------------48-------

	vec3 specular;
	float pad12;

	//------------64-------

	float constant;
	float linear;
	float quadratic;
	float pad13;

	//------------80-------
};

struct DirectionalLightSource
{
	vec3 direction;
	float pad14;

	//-----------16-------

	vec3 ambient;
	float pad10;

	//------------32-------

	vec3 diffuse;
	float pad11;

	//------------48-------

	vec3 specular;
	float pad12;

	//------------64-------
};

struct Material
{
	sampler2D diffuseTexture;
	sampler2D specularTexture;
	sampler2D normalTexture;
	sampler2D heightTexture;
	sampler2D ambientOcclusionTexture;
	sampler2D roughnessTexture;
	sampler2D emissiveTexture;
	float shininess;
};

in vec2 pass_uv;
in vec3 pass_normal;
in vec3 fragPosition;

out vec4 pixelColor;

uniform int lightCount;
uniform vec3 viewPos;
uniform Material material;

layout(std430, binding = 1) buffer SpotLightBuffer
{
   SpotLightSource spotLightSources[];
};

layout(std430, binding = 2) buffer PointLightBuffer
{
   PointLightSource pointLightSources[];
};

layout(std430, binding = 3) buffer DirectionalLightBuffer
{
   DirectionalLightSource directionalLightSources[];
};

vec3 CalculateDirectionalLight(DirectionalLightSource dirLight, vec3 normal, vec3 viewDir)
{
	vec3 lightDir = normalize(-dirLight.direction);

	float diffuse = max(dot(normal, lightDir), 0.0);

	vec3 reflectDir = reflect(-lightDir, normal);
	float specular = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);

	return vec3(0.0);
}

void main() {
	vec3 ambient = texture(material.diffuseTexture, pass_uv).rgb * spotLightSources[0].ambient;

	vec3 fragNormal = normalize(pass_normal);
	vec3 L = normalize(spotLightSources[0].position - fragPosition);
	float diff = max(dot(fragNormal, L), 0.0);
	vec3 diffuse = diff * spotLightSources[0].diffuse * texture(material.diffuseTexture, pass_uv).rgb;

	//specular
	vec3 viewDir = normalize(viewPos - fragPosition);
	vec3 reflectDir = reflect(-L, fragNormal); 
	float spec = pow(max(dot(viewDir, reflectDir), 0.0), 16);
	vec3 specular = texture(material.specularTexture, pass_uv).rgb * spec * spotLightSources[0].specular;

	float theta = dot(L, normalize(-spotLightSources[0].direction));
	float epsilon = spotLightSources[0].cutOff - spotLightSources[0].outerCutOff;
	float intensity = clamp((theta - spotLightSources[0].outerCutOff) / epsilon, 0.0, 1.0);
	diffuse *= intensity;
	specular *= intensity;


	float distance = length(spotLightSources[0].position - fragPosition);
	float attenuation = 1.0 / (spotLightSources[0].constant + spotLightSources[0].linear * distance + spotLightSources[0].quadratic * (distance * distance));
	ambient *= attenuation; 
	diffuse *= attenuation;
	specular *= attenuation; 


    pixelColor = vec4(ambient + diffuse + specular, 1.0);
	//pixelColor = vec4(vec3((theta+1.0)/2.0), 1.0);
}