#version 300 es

layout(location = 0) in vec3 vertexPosition_modelspace;
layout(location = 1) in vec2 vertexUV;

uniform vec2 screen;
uniform vec2 size;

out highp vec2 uv;

void main()
{
  gl_Position.xyz = vertexPosition_modelspace;
  gl_Position.w   = 1.0;
  gl_Position.xy *= screen.xy * size.xy;
  gl_Position.x  -= 1.0 - (screen.x * size.x);
  gl_Position.y  -= 1.0 - (screen.y * size.y);
  gl_Position.x  += screen.x * 10.0;
  gl_Position.y  += screen.y * 10.0;

  uv = vertexUV;
}
