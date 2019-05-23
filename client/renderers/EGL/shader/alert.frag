#version 300 es

in  highp vec2 uv;
in  highp vec2 sz;
out highp vec4 color;

uniform sampler2D sampler1;

void main()
{
  color = texelFetch(sampler1, ivec2(uv * sz), 0);
}
