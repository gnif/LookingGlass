#version 300 es
precision mediump float;

#include "compat.h"

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D texture;
uniform vec2      uOutRes;
uniform uvec4     uConsts[4];

#define A_GPU  1
#define A_GLSL 1
#define A_FULL 1

#include "ffx_a.h"

#define FSR_EASU_F 1

vec4 _textureGather(sampler2D tex, vec2 uv, int comp)
{
  vec2 res = vec2(textureSize(tex, 0));
  ivec2 p = ivec2((uv * res) - 0.5f);
  vec4 c0 = texelFetchOffset(tex, p, 0, ivec2(0,1));
  vec4 c1 = texelFetchOffset(tex, p, 0, ivec2(1,1));
  vec4 c2 = texelFetchOffset(tex, p, 0, ivec2(1,0));
  vec4 c3 = texelFetchOffset(tex, p, 0, ivec2(0,0));
  return vec4(c0[comp], c1[comp], c2[comp],c3[comp]);
}

AF4 FsrEasuRF(AF2 p){return AF4(_textureGather(texture, p, 0));}
AF4 FsrEasuGF(AF2 p){return AF4(_textureGather(texture, p, 1));}
AF4 FsrEasuBF(AF2 p){return AF4(_textureGather(texture, p, 2));}

#include "ffx_fsr1.h"

void main()
{
  vec3 color;
  uvec2 point = uvec2(fragCoord * uOutRes);
  FsrEasuF(color, point, uConsts[0], uConsts[1], uConsts[2], uConsts[3]);
  fragColor = vec4(color.rgb, 1);
}
