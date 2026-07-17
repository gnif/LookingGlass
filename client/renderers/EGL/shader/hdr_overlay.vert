#version 300 es
precision highp float;

layout(location = 0) in vec3 vertex;
layout(location = 1) in vec2 coord;

out vec2 fragCoord;

void main()
{
  gl_Position = vec4(vertex, 1.0);
  fragCoord = coord;
}
