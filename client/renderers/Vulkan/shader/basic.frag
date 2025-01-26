#version 460

layout (location = 0) in vec2 uv;

layout (location = 0) out vec4 color;

void main()
{
  color = vec4(uv, 0.0, 1.0);
}
