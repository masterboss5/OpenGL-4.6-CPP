#version 460 core

in vec4 gizmoColor;
layout(location = 0) out vec4 outputColor;

void main()
{
	outputColor = gizmoColor;
}
