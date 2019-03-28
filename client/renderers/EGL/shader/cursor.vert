#version 300 es

layout(location = 0) in vec3 vertexPosition_modelspace;
layout(location = 1) in vec2 vertexUV;

uniform vec4 mouse;

out highp vec2 uv;

void main()
{
  gl_Position.xyz = vertexPosition_modelspace;
  gl_Position.w   = 1.0;

  gl_Position.x += 1.0f;
  gl_Position.y -= 1.0f;

  gl_Position.x *= mouse.z;
  gl_Position.y *= mouse.w;

  gl_Position.x += mouse.x;
  gl_Position.y -= mouse.y;

  uv = vertexUV;
}
