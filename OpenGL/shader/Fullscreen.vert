#version 460 core

out gl_PerVertex
{
    vec4 gl_Position;
};

const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
out vec2 textureCoordinate;

void main()
{
    vec2 position = positions[gl_VertexID];
    textureCoordinate = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
