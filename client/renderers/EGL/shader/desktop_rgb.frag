#version 300 es

in  highp vec2 uv;
out highp vec4 color;

uniform sampler2D sampler1;

uniform       int   nearest;
uniform highp vec2  size;

uniform       int   nv;
uniform highp float nvGain;

void main()
{
  if(nearest == 1)
    color = texture(sampler1, uv);
  else
    color = texelFetch(sampler1, ivec2(uv * size), 0);

  if (nv == 1)
  {
    highp float lumi = 1.0 - (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b);
    color *= 1.0 + lumi;
    color *= nvGain;
  }
}
