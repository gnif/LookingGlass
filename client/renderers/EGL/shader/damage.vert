#version 300 es

layout(location = 0) in vec2 vertex;

uniform mat3x2 transform;

void main()
{
  gl_Position = vec4(transform * vec3(vertex, 1.0), 0.0, 1.0);
}
