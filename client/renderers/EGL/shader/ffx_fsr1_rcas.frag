#version 300 es
precision mediump float;

#include "compat.h"

in  vec2  iFragCoord;
out vec4  fragColor;

uniform sampler2D iChannel0;
uniform uvec2     uInRes[8];
uniform float     uSharpness;

#define A_GPU  1
#define A_GLSL 1
#define A_FULL 1

#include "ffx_a.h"

AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(iChannel0, ASU2(p), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#define FSR_RCAS_F       1
#define FSR_RCAS_DENOISE 1
#include "ffx_fsr1.h"

void main()
{
  vec2 inRes  = vec2(uInRes[0]);
  uvec2 point = uvec2(iFragCoord * (inRes + 0.5f));

  uvec4 const0;
  FsrRcasCon(const0, uSharpness);

  FsrRcasF(fragColor.r, fragColor.g, fragColor.b, point, const0);
  fragColor.a = 1.0f;
}
