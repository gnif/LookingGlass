#version 300 es

layout(location = 0) in vec3 vertexPosition_modelspace;

uniform float scale;

void main()
{
  gl_Position.xyz = vertexPosition_modelspace;
  gl_Position.y  *= scale;
  gl_Position.w   = 1.0;
}
