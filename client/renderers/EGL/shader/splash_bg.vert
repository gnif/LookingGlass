#version 300 es
precision mediump float;

layout(location = 0) in vec3 vertexPosition_modelspace;

out vec3 pos;

void main()
{
  gl_Position.xyz = vertexPosition_modelspace; 
  gl_Position.w   = 1.0;

  pos = vertexPosition_modelspace;
}
