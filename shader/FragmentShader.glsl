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

uniform int spotLightSourcesCount;
layout(std430, binding = 1) buffer SpotLightBuffer
{
   SpotLightSource spotLightSources[];
};

uniform int pointLightSourcesCount;
layout(std430, binding = 2) buffer PointLightBuffer
{
   PointLightSource pointLightSources[];
};

uniform int directionalLightSourcesCount;
layout(std430, binding = 3) buffer DirectionalLightBuffer
{
   DirectionalLightSource directionalLightSources[];
};

vec3 CalculateSpotLight(SpotLightSource spotLight, vec3 normal, vec3 viewDir)
{
	vec3 lightDir = normalize(spotLight.position - fragPosition);
	float diff = max(dot(normal, lightDir), 0.0);
	vec3 reflectDir = reflect(-lightDir, normal);
	float spec = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);

	float distance = length(spotLight.position - fragPosition);
	float attenuation = 1.0 / (spotLight.constant + spotLight.linear * distance + spotLight.quadratic * (distance * distance));

	vec3 ambient = spotLight.ambient * vec3(texture(material.diffuseTexture, pass_uv));
	vec3 diffuse = spotLight.diffuse * diff * vec3(texture(material.diffuseTexture, pass_uv));
	vec3 specular = spotLight.specular * spec * vec3(texture(material.specularTexture, pass_uv));

	float theta = dot(lightDir, normalize(-spotLight.direction)); 
    float epsilon = spotLight.cutOff - spotLight.outerCutOff;
    float intensity = clamp((theta - spotLight.outerCutOff) / epsilon, 0.0, 1.0);

	ambient *= attenuation * intensity;
    diffuse *= attenuation * intensity;
    specular *= attenuation * intensity;

    return (ambient + diffuse + specular);
}

vec3 CalculateDirectionalLight(DirectionalLightSource dirLight, vec3 normal, vec3 viewDir)
{




	vec3 lightDir = normalize(-dirLight.direction);

	float diff = max(dot(normal, lightDir), 0.0);
	vec3 reflectDir = reflect(-lightDir, normal);
	float spec = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);

	vec3 ambient = dirLight.ambient * vec3(texture(material.diffuseTexture, pass_uv));
	vec3 diffuse = dirLight.diffuse * diff * vec3(texture(material.diffuseTexture, pass_uv));
	vec3 specular = dirLight.specular * spec * vec3(texture(material.specularTexture, pass_uv));

	return vec3(ambient + diffuse + specular);
}

vec3 CalculatePointLight(PointLightSource pointLight, vec3 normal, vec3 viewDir)
{
	vec3 lightDir = normalize(pointLight.position - fragPosition);
	float diff = max(dot(normal, lightDir), 0.0);
	vec3 reflectDir = reflect(-lightDir, normal);
	float spec = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);
	float distance = length(pointLight.position - fragPosition);
	float attenuation = 1.0 / (pointLight.constant + pointLight.linear * distance + pointLight.quadratic * (distance * distance));

	vec3 ambient = pointLight.ambient * vec3(texture(material.diffuseTexture, pass_uv));
	vec3 diffuse = pointLight.diffuse * diff * vec3(texture(material.diffuseTexture, pass_uv));
	vec3 specular = pointLight.specular * spec * vec3(texture(material.specularTexture, pass_uv));
	ambient *= attenuation;
	diffuse *= attenuation;
	specular *= attenuation;

	return vec3(ambient + diffuse + specular);
}

// void main()
//{
// 	vec3 ambient = spotLightSources[0].ambient * texture(material.diffuseTexture, pass_uv).rgb;

// 	vec3 fragNormal = normalize(pass_normal);
// 	vec3 lightDirection = normalize(spotLightSources[0].position - fragPosition);
// 	float diff = max(dot(fragNormal, lightDirection), 0.0);
// 	vec3 diffuse = spotLightSources[0].diffuse * diff * texture(material.diffuseTexture, pass_uv).rgb;

// 	//specular
// 	vec3 viewDir = normalize(viewPos - fragPosition);
// 	vec3 reflectDir = reflect(-lightDirection, fragNormal); 
// 	float spec = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);
// 	vec3 specular = texture(material.specularTexture, pass_uv).rgb * spec * spotLightSources[0].specular;

// 	float theta = dot(lightDirection, normalize(-spotLightSources[0].direction));
// 	float epsilon = spotLightSources[0].cutOff - spotLightSources[0].outerCutOff;
// 	float intensity = clamp((theta - spotLightSources[0].outerCutOff) / epsilon, 0.0, 1.0);
// 	diffuse *= intensity;
// 	specular *= intensity;


// 	float distance = length(spotLightSources[0].position - fragPosition);
// 	float attenuation = 1.0 / (spotLightSources[0].constant + spotLightSources[0].linear * distance + spotLightSources[0].quadratic * (distance * distance));
// 	ambient *= attenuation; 
// 	diffuse *= attenuation;
// 	specular *= attenuation; 


//     pixelColor = vec4(ambient + diffuse + specular, 1.0);
// }

void main()
{
	vec3 normal = normalize(pass_normal);
	vec3 viewDir = normalize(viewPos - fragPosition);

	//vec3 result = CalculateDirectionalLight(directionalLightSources[0], normal, viewDir);
	//result += CalculateSpotLight(spotLightSources[0], normal, viewDir);
	// for (int i = 0; i < pointLightSourcesCount; i++)
	// {
	// 	result += CalculatePointLight(pointLightSources[i], normal, viewDir);
	// }


	//New version
	vec3 result = vec3(0, 0, 0);

	for (int i = 0; i < pointLightSourcesCount; i++)
	{
		result += CalculatePointLight(pointLightSources[i], normal, viewDir);
	}
	
	for (int i = 0; i < spotLightSourcesCount; i++)
	{
		result += CalculateSpotLight(spotLightSources[i], normal, viewDir);
	}

	for (int i = 0; i < directionalLightSourcesCount; i++)
	{
		result += CalculateDirectionalLight(directionalLightSources[i], normal, viewDir);
	}

	pixelColor = vec4(result, 1.0);
}