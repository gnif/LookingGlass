#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;
precision highp int;

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
uniform float mapHDRContentPeak;
uniform bool  mapHDRPQ;
uniform bool  outputHDRLinear;

vec3 samplePQLinear(vec2 coord)
{
  ivec2 size      = textureSize(sampler1, 0);
  vec2  samplePos = coord * vec2(size) - 0.5;
  ivec2 base      = ivec2(floor(samplePos));
  vec2  f         = fract(samplePos);
  ivec2 limit     = size - ivec2(1);

  vec3 c00 = pq2lin(texelFetch(sampler1,
      clamp(base, ivec2(0), limit), 0).rgb, 1.0);
  vec3 c10 = pq2lin(texelFetch(sampler1,
      clamp(base + ivec2(1, 0), ivec2(0), limit), 0).rgb, 1.0);
  vec3 c01 = pq2lin(texelFetch(sampler1,
      clamp(base + ivec2(0, 1), ivec2(0), limit), 0).rgb, 1.0);
  vec3 c11 = pq2lin(texelFetch(sampler1,
      clamp(base + ivec2(1, 1), ivec2(0), limit), 0).rgb, 1.0);

  vec3 linear = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
  return max(linear, 0.0);
}

void main()
{
  bool sampledPQLinear = false;
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
      if (isHDR && mapHDRPQ)
      {
        vec3 linear     = samplePQLinear(uv);
        color           = vec4(
            outputHDRLinear ? linear : lin2pq(linear), 1.0);
        sampledPQLinear = outputHDRLinear;
      }
      else
        color = texture(sampler1, uv);
      break;
    }
  }

  if (isHDR && mapHDRtoSDR)
    color.rgb = mapToSDR(color.rgb, mapHDRGain,
        mapHDRContentPeak, mapHDRPQ);
  else if (isHDR && outputHDRLinear && mapHDRPQ)
  {
    vec3 linear2020 = sampledPQLinear ?
      color.rgb : pq2lin(color.rgb, 1.0);
    color.rgb = bt2020to709(linear2020) * 125.0;
  }

  // The legacy effects are defined for an SDR signal. Do not apply them to a
  // native HDR signal where their nonlinear operations would alter luminance
  // and gamut incorrectly. They remain available after HDR-to-SDR mapping.
  if (cbMode > 0 && (!isHDR || mapHDRtoSDR))
    color = cbTransform(color, cbMode);

  if (nvGain > 0.0 && (!isHDR || mapHDRtoSDR))
  {
    highp float lumi = (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b);
    if (lumi < 0.5)
      color *= atanh((1.0 - lumi) * 2.0 - 1.0) + 1.0;
    color *= nvGain;
  }

  color.a = 1.0;
}
