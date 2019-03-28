#version 300 es

layout(location = 0) in vec3 vertexPosition_modelspace;
layout(location = 1) in vec2 vertexUV;

uniform vec4 position;

out highp vec2 uv;

void main()
{
  gl_Position.xyz = vertexPosition_modelspace; 
  gl_Position.w   = 1.0;
  gl_Position.x  -= position.x;
  gl_Position.y  -= position.y;
  gl_Position.x  *= position.z;
  gl_Position.y  *= position.w;

  uv = vertexUV;
}
