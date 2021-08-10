#version 300 es
precision mediump float;

#include "compat.h"

in  vec2  iFragCoord;
out vec4  fragColor;

uniform sampler2D iChannel0;
uniform uvec2     uInRes[8];
uniform uvec2     uOutRes;
uniform float     uSharpness;

#define A_GPU 1
#define A_GLSL 1

#include "ffx_a.h"

vec3 imageLoad(ivec2 point)
{
  return texelFetch(iChannel0, point, 0).rgb;
}

AF3 CasLoad(ASU2 p)
{
  return imageLoad(p).rgb;
}

void CasInput(inout AF1 r,inout AF1 g,inout AF1 b) {}

#include "ffx_cas.h"

void main()
{
  uvec2 point = uvec2(iFragCoord * vec2(uInRes[0].xy));
   
  vec4 color;
  vec2 inputResolution  = vec2(uInRes[0]);
  vec2 outputResolution = vec2(uOutRes);

  uvec4 const0;
  uvec4 const1;

  CasSetup(const0, const1, uSharpness,
    inputResolution.x, inputResolution.y, 
    outputResolution.x, outputResolution.y);

  CasFilter(
    fragColor.r, fragColor.g, fragColor.b,
    point,
    const0, const1,
    true);
}
