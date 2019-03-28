#version 300 es

in  highp vec2 uv;
out highp vec4 color;

uniform sampler2D sampler1;
uniform int nv;
uniform highp float nvGain;

void main()
{
  color = texture(sampler1, uv);
  if (nv == 1)
    color *= nvGain;
}
