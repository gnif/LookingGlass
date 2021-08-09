#version 300 es

#define EGL_SCALE_AUTO    0
#define EGL_SCALE_NEAREST 1
#define EGL_SCALE_LINEAR  2
#define EGL_SCALE_MAX     3

#include "color_blind.h"

in  highp vec2 uv;
out highp vec4 color;

uniform sampler2D sampler1;

uniform       int   scaleAlgo;
uniform highp vec2  size;
uniform highp float textureScale;

uniform highp float nvGain;
uniform       int   cbMode;

void main()
{
  switch (scaleAlgo)
  {
    case EGL_SCALE_NEAREST:
      color = texelFetch(sampler1, ivec2(uv * size * textureScale), 0);
      break;

    case EGL_SCALE_LINEAR:
      color = texture(sampler1, uv);
      break;
  }

  if (cbMode > 0)
    color = cbTransform(color, cbMode);

  if (nvGain > 0.0)
  {
    highp float lumi = 1.0 - (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b);
    color *= 1.0 + lumi;
    color *= nvGain;
  }

  color.a = 1.0;
}
