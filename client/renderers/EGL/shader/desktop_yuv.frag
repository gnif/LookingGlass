#version 300 es

in  highp vec2 uv;
out highp vec4 color;

uniform       int   nearest;
uniform highp vec2  size;

uniform       int   nv;
uniform highp float nvGain;

uniform sampler2D sampler1;
uniform sampler2D sampler2;
uniform sampler2D sampler3;

void main()
{
  highp vec4 yuv;
  if(nearest == 1)
  {
    yuv = vec4(
      texture(sampler1, uv).r,
      texture(sampler2, uv).r,
      texture(sampler3, uv).r,
      1.0
    );
  }
  else
  {
    highp ivec2 px = ivec2(uv * size);
    yuv = vec4(
      texelFetch(sampler1, px, 0).r,
      texelFetch(sampler2, px, 0).r,
      texelFetch(sampler3, px, 0).r,
      1.0
    );
  }
  
  highp mat4 yuv_to_rgb = mat4(
    1.0,  0.0  ,  1.402, -0.701,
    1.0, -0.344, -0.714,  0.529,
    1.0,  1.772,  0.0  , -0.886,
    1.0,  1.0  ,  1.0  ,  1.0
  );
  
  color = yuv * yuv_to_rgb;
  if (nv == 1)
  {
    highp float lumi = 1.0 - (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b);
    color *= 1.0 + lumi;
    color *= nvGain;
  }
}
