#version 300 es

layout(location = 0) in vec3 vertexPosition_modelspace;

out highp vec3  pos;
out highp float a;

void main()
{
  gl_Position.xyz = vertexPosition_modelspace; 
  gl_Position.w   = 1.0;

  pos = vertexPosition_modelspace;
}
