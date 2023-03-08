#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;

#include "compat.h"

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D sampler1;
uniform uvec4     uConsts[2];

#define A_GPU 1
#define A_GLSL 1

#include "ffx_a.h"

vec3 imageLoad(ivec2 point)
{
  return texelFetch(sampler1, point, 0).rgb;
}

AF3 CasLoad(ASU2 p)
{
  return imageLoad(p).rgb;
}

void CasInput(inout AF1 r,inout AF1 g,inout AF1 b) {}

#include "ffx_cas.h"

void main()
{
  vec2  res   = vec2(textureSize(sampler1, 0));
  uvec2 point = uvec2(fragCoord * res);

  CasFilter(
    fragColor.r, fragColor.g, fragColor.b,
    point, uConsts[0], uConsts[1], true);
}
