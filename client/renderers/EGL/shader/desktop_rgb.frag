#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;

#define EGL_SCALE_AUTO    0
#define EGL_SCALE_NEAREST 1
#define EGL_SCALE_LINEAR  2
#define EGL_SCALE_MAX     3

#include "color_blind.h"
#include "hdr.h"

in  vec2 uv;
out vec4 color;

uniform sampler2D sampler1;

uniform int   scaleAlgo;

uniform float nvGain;
uniform int   cbMode;
uniform bool  isHDR;
uniform bool  mapHDRtoSDR;
uniform float mapHDRGain;
uniform bool  mapHDRPQ;

void main()
{
  switch (scaleAlgo)
  {
    case EGL_SCALE_NEAREST:
    {
      vec2 ts = vec2(textureSize(sampler1, 0));
      color   = texelFetch(sampler1, ivec2(uv * ts), 0);
      break;
    }

    case EGL_SCALE_LINEAR:
    {
      color = texture(sampler1, uv);
      break;
    }
  }

  if (isHDR && mapHDRtoSDR)
    color.rgb = mapToSDR(color.rgb, mapHDRGain, mapHDRPQ);

  if (cbMode > 0)
    color = cbTransform(color, cbMode);

  if (nvGain > 0.0)
  {
    highp float lumi = (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b);
    if (lumi < 0.5)
      color *= atanh((1.0 - lumi) * 2.0 - 1.0) + 1.0;
    color *= nvGain;
  }

  color.a = 1.0;
}
