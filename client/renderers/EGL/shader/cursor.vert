#version 300 es

layout(location = 0) in vec3 vertexPosition_modelspace;
layout(location = 1) in vec2 vertexUV;

uniform vec4 mouse;
uniform lowp int rotate;

out highp vec2 uv;

void main()
{
  vec2 muv = vertexPosition_modelspace.xy;
  muv.x += 1.0f;
  muv.y -= 1.0f;
  muv.x *= mouse.z;
  muv.y *= mouse.w;
  muv.x += mouse.x;
  muv.y -= mouse.y;

  if (rotate == 0) // 0
  {
    gl_Position.xy = muv;
  }
  else if (rotate == 1) // 90
  {
    gl_Position.x =  muv.y;
    gl_Position.y = -muv.x;
  }
  else if (rotate == 2) // 180
  {
    gl_Position.x = -muv.x;
    gl_Position.y = -muv.y;
  }
  else if (rotate == 3) // 270
  {
    gl_Position.x = -muv.y;
    gl_Position.y =  muv.x;
  }

  gl_Position.w = 1.0;
  uv = vertexUV;
}
