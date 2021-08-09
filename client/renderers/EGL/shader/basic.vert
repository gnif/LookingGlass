#version 300 es

precision mediump float;

layout(location = 0) in vec2 uVertex;
layout(location = 1) in vec2 uUV;

out vec2 iFragCoord;

void main()
{
  gl_Position.xy = uVertex;
  iFragCoord     = uUV;
}
