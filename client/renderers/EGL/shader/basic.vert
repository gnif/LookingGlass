#version 300 es
precision mediump float;

layout(location = 0) in vec2 vertex;
out vec2 fragCoord;

uniform vec2   desktopSize;
uniform mat3x2 transform;

void main()
{
  vec2 pos    = transform * vec3(vertex, 1.0);
  gl_Position = vec4(pos.x, -pos.y, 0.0, 1.0);
  fragCoord   = vertex / desktopSize;
}
