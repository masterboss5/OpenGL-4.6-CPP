#version 460 core

flat in vec4 lineColor;
layout(location = 0) out vec4 outputColor;

void main()
{
	outputColor = lineColor;
}
