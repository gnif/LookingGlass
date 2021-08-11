#version 300 es
precision mediump float;

#include "compat.h"

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D texture;
uniform uvec4     uConsts;

#define A_GPU  1
#define A_GLSL 1
#define A_FULL 1

#include "ffx_a.h"

AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(texture, ASU2(p), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#define FSR_RCAS_F       1
#define FSR_RCAS_DENOISE 1
#include "ffx_fsr1.h"

void main()
{
  vec2  inRes = vec2(textureSize(texture, 0));
  uvec2 point = uvec2(fragCoord * (inRes + 0.5f));

  FsrRcasF(fragColor.r, fragColor.g, fragColor.b, point, uConsts);
  fragColor.a = 1.0f;
}
