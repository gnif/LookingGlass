#version 300 es

layout(location = 0) in vec3 vertexPosition_modelspace;

uniform vec2 scale;

out highp float a;

void main()
{
  gl_Position.xyz = vertexPosition_modelspace;
  gl_Position.y  *= scale.y;
  gl_Position.w   = 1.0;

  a = scale.x;
}
