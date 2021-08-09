#version 300 es

layout(location = 0) in vec2 vertex;
out highp vec2 uv;

uniform highp vec2 size;
uniform mat3x2 transform;

void main()
{
  highp vec2 uvScale;

  gl_Position = vec4(transform * vec3(vertex, 1.0), 0.0, 1.0);
  uvScale.x = 1.0 / size.x;
  uvScale.y = 1.0 / size.y;
  uv = vertex * uvScale;
}
