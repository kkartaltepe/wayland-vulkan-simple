#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec3 inCol;

layout(binding = 0) uniform Opaque {
	mat4 m;
} mtx;


layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = mtx.m * vec4(inPos, 0.0, 1.0);
    fragColor = inCol;
}
