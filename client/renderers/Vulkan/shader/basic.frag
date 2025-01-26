#version 460

layout(binding = 0) uniform sampler2D sampler1;

layout (location = 0) in vec2 uv;

layout (location = 0) out vec4 color;

void main()
{
  color = texture(sampler1, uv);
}
