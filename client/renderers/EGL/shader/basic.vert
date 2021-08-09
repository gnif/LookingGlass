#version 300 es

precision mediump float;

layout(location = 0) in vec2 uVertex;
layout(location = 1) in vec2 uUV;

out vec2 iFragCoord;

void main()
{
  gl_Position = vec4(uVertex, 0.0, 1.0);
  iFragCoord  = uUV;
}
