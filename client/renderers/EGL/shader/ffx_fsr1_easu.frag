#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;

#include "compat.h"

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D sampler1;
uniform vec2      uOutRes;
uniform uvec4     uConsts[4];

#define A_GPU  1
#define A_GLSL 1
#define A_FULL 1

#include "ffx_a.h"

#define FSR_EASU_F 1

AF4 FsrEasuRF(AF2 p){return AF4(textureGather(sampler1, p, 0));}
AF4 FsrEasuGF(AF2 p){return AF4(textureGather(sampler1, p, 1));}
AF4 FsrEasuBF(AF2 p){return AF4(textureGather(sampler1, p, 2));}

#include "ffx_fsr1.h"

void main()
{
  vec3 color;
  uvec2 point = uvec2(fragCoord * uOutRes);
  FsrEasuF(color, point, uConsts[0], uConsts[1], uConsts[2], uConsts[3]);
  fragColor = vec4(color.rgb, 1);
}
