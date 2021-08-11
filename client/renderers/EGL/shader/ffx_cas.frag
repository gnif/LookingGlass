#version 300 es
precision mediump float;

#include "compat.h"

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D texture;
uniform float     uSharpness;

#define A_GPU 1
#define A_GLSL 1

#include "ffx_a.h"

vec3 imageLoad(ivec2 point)
{
  return texelFetch(texture, point, 0).rgb;
}

AF3 CasLoad(ASU2 p)
{
  return imageLoad(p).rgb;
}

void CasInput(inout AF1 r,inout AF1 g,inout AF1 b) {}

#include "ffx_cas.h"

void main()
{
  vec2  res   = vec2(textureSize(texture, 0));
  uvec2 point = uvec2(fragCoord * res);
   
  vec4 color;
  uvec4 const0;
  uvec4 const1;

  CasSetup(const0, const1, uSharpness,
    res.x, res.y, res.x, res.y);

  CasFilter(
    fragColor.r, fragColor.g, fragColor.b,
    point,
    const0, const1,
    true);
}
