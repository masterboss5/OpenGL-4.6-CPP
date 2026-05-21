#version 460 core
#extension GL_ARB_gpu_shader_int64 : enable

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

out vec2 pass_uv;
out vec3 pass_normal;
out vec3 fragPosition;

layout(std430, binding = 0) buffer InstanceData {
    mat4 transforms[];
};

uniform mat4 projection;
uniform mat4 view;

void main() {
    gl_Position = projection * view * transforms[gl_InstanceID] * vec4(position, 1.0);
    fragPosition = vec3(transforms[gl_InstanceID] * vec4(position, 1.0));
    pass_uv = uv;
    pass_normal = mat3(transpose(inverse(transforms[gl_InstanceID]))) * normal;
}